#include "esp_pluto_sdr.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fault(pluto_client *c, int e) {
  c->faulted = true;
  c->streaming = false;
  return e;
}
static int valid(pluto_client *c) {
  return c && c->io.read && c->io.write && !c->faulted;
}
static int token(const char *s) {
  if (!s || !*s || strlen(s) > 63)
    return 0;
  for (; *s; s++)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
          (*s >= '0' && *s <= '9') || strchr("_:-.", *s)))
      return 0;
  return 1;
}
static int transfer(pluto_client *c, void *b, size_t n, bool writing) {
  size_t p = 0;
  while (p < n) {
    int r = writing ? c->io.write(c->io.user, (char *)b + p, n - p)
                    : c->io.read(c->io.user, (char *)b + p, n - p);
    if (r <= 0 || (size_t)r > n - p)
      return fault(c, PLUTO_IO);
    p += (size_t)r;
  }
  return 0;
}
static int sendcmd(pluto_client *c, const char *s) {
  return transfer(c, (void *)s, strlen(s), true);
}
static int line(pluto_client *c, char *b, size_t cap) {
  size_t n = 0;
  while (n + 1 < cap) {
    char x;
    int r = transfer(c, &x, 1, false);
    if (r)
      return r;
    if (x == '\n') {
      if (n && b[n - 1] == '\r')
        n--;
      b[n] = 0;
      return 0;
    }
    b[n++] = x;
  }
  return fault(c, PLUTO_PROTOCOL);
}
static int number(pluto_client *c, int *out) {
  char b[32], *end;
  int r = line(c, b, sizeof b);
  if (r)
    return r;
  errno = 0;
  long v = strtol(b, &end, 10);
  if (errno || end == b || *end || v < INT_MIN || v > INT_MAX)
    return fault(c, PLUTO_PROTOCOL);
  *out = (int)v;
  return 0;
}
static int status(pluto_client *c) {
  int v, r = number(c, &v);
  if (r)
    return r;
  return v < 0 ? v : (v ? fault(c, PLUTO_PROTOCOL) : 0);
}
void pluto_init(pluto_client *c, pluto_io io) {
  if (c) {
    memset(c, 0, sizeof *c);
    c->io = io;
  }
}
int pluto_context(pluto_client *c, char *b, size_t cap, size_t *out) {
  if (out)
    *out = 0;
  if (!valid(c) || !b || !out || !cap)
    return PLUTO_INVALID;
  if (c->streaming)
    return PLUTO_STATE;
  int r = sendcmd(c, "PRINT\r\n"), n;
  if (r)
    return r;
  r = number(c, &n);
  if (r)
    return r;
  if (n < 0)
    return n;
  if ((size_t)n >= cap)
    return fault(c, PLUTO_PROTOCOL);
  r = transfer(c, b, (size_t)n, false);
  if (r)
    return r;
  char x;
  r = transfer(c, &x, 1, false);
  if (r)
    return r;
  if (x != '\n')
    return fault(c, PLUTO_PROTOCOL);
  b[n] = 0;
  *out = (size_t)n;
  return 0;
}
int pluto_rx_open(pluto_client *c, const char *dev, size_t count,
                  uint32_t mask) {
  if (!valid(c) || !token(dev) || !count || count > INT_MAX || !mask)
    return PLUTO_INVALID;
  if (c->streaming)
    return PLUTO_STATE;
  char cmd[160];
  snprintf(cmd, sizeof cmd, "OPEN %s %lu %08x\r\n", dev, (unsigned long)count,
           (unsigned)mask);
  int r = sendcmd(c, cmd);
  if (!r)
    r = status(c);
  if (!r) {
    strcpy(c->device, dev);
    c->mask = mask;
    c->streaming = true;
  }
  return r;
}
int pluto_rx_read(pluto_client *c, void *b, size_t cap, size_t *out) {
  if (out)
    *out = 0;
  if (!valid(c) || !b || !out || !cap || cap > INT_MAX)
    return PLUTO_INVALID;
  if (!c->streaming)
    return PLUTO_STATE;
  char cmd[128];
  snprintf(cmd, sizeof cmd, "READBUF %s %lu\r\n", c->device,
           (unsigned long)cap);
  int r = sendcmd(c, cmd);
  if (r)
    return r;
  bool first = true;
  while (*out < cap) {
    int n;
    r = number(c, &n);
    if (r)
      return r;
    if (n < 0)
      return fault(c, n);
    if (!n)
      break;
    if ((size_t)n > cap - *out)
      return fault(c, PLUTO_PROTOCOL);
    if (first) {
      char mask[16], expected[16];
      r = line(c, mask, sizeof mask);
      if (r)
        return r;
      snprintf(expected, sizeof expected, "%08x", (unsigned)c->mask);
      if (strcmp(mask, expected))
        return fault(c, PLUTO_PROTOCOL);
      first = false;
    }
    r = transfer(c, (char *)b + *out, (size_t)n, false);
    if (r)
      return r;
    *out += (size_t)n;
    c->bytes_received += (size_t)n;
  }
  return 0;
}
int pluto_rx_close(pluto_client *c) {
  if (!valid(c))
    return PLUTO_INVALID;
  if (!c->streaming)
    return PLUTO_STATE;
  char cmd[96];
  snprintf(cmd, sizeof cmd, "CLOSE %s\r\n", c->device);
  int r = sendcmd(c, cmd);
  if (!r)
    r = status(c);
  if (r)
    return fault(c, r);
  c->streaming = false;
  return 0;
}
int pluto_attr_write(pluto_client *c, const char *dev, const char *dir,
                     const char *ch, const char *attr, const char *value) {
  if (!valid(c) || !token(dev) || !token(ch) || !token(attr) || !value ||
      strlen(value) > 128 || !dir ||
      (strcmp(dir, "INPUT") && strcmp(dir, "OUTPUT")))
    return PLUTO_INVALID;
  if (c->streaming)
    return PLUTO_STATE;
  for (const char *p = value; *p; p++)
    if (!strchr("0123456789+-.abcdefghijklmnopqrstuvwxyz_", *p))
      return PLUTO_INVALID;
  char cmd[256];
  size_t n = strlen(value);
  snprintf(cmd, sizeof cmd, "WRITE %s %s %s %s %lu\r\n", dev, dir, ch, attr,
           (unsigned long)n);
  int r = sendcmd(c, cmd);
  if (!r)
    r = transfer(c, (void *)value, n, true);
  if (r)
    return r;
  int result;
  r = number(c, &result);
  if (r)
    return r;
  if (result < 0)
    return result;
  return (size_t)result == n ? 0 : fault(c, PLUTO_PROTOCOL);
}
