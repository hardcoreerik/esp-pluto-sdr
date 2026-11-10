#include "pluto_iiod.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int io_send_str(pluto_iiod_t *c, const char *s) { return c->io.send(c->io.ctx, s, strlen(s)); }

/* Reads one LF-terminated line (control traffic only; one byte per recv). */
static int read_line(pluto_iiod_t *c, char *out, size_t cap) {
  size_t n = 0;
  for (;;) {
    char ch;
    if (c->io.recv(c->io.ctx, &ch, 1) != 1) return PLUTO_ERR_IO;
    if (ch == '\n') break;
    if (n + 1 < cap) out[n++] = ch;
  }
  out[n] = 0;
  while (n && (out[n - 1] == '\r')) out[--n] = 0;
  return (int)n;
}

/* A command's reply line is a decimal status/length; negative = iiod error. */
static int read_status(pluto_iiod_t *c, long *v) {
  char l[40];
  if (read_line(c, l, sizeof l) < 0) return PLUTO_ERR_IO;
  char *end;
  long x = strtol(l, &end, 10);
  if (end == l) return PLUTO_ERR_PROTO;
  *v = x;
  return PLUTO_OK;
}

static int command(pluto_iiod_t *c, const char *line, long *v) {
  char buf[160];
  int n = snprintf(buf, sizeof buf, "%s\r\n", line);
  if (n <= 0 || (size_t)n >= sizeof buf) return PLUTO_ERR_RANGE;
  if (c->io.send(c->io.ctx, buf, (size_t)n) != n) return PLUTO_ERR_IO;
  return read_status(c, v);
}

static int discard(pluto_iiod_t *c, size_t n) {
  char tmp[128];
  while (n) {
    size_t k = n < sizeof tmp ? n : sizeof tmp;
    if (c->io.recv(c->io.ctx, tmp, k) != (int)k) return PLUTO_ERR_IO;
    n -= k;
  }
  return PLUTO_OK;
}

/* Streams the PRINT xml, keeping only `<device id="X" name="Y"` pairs for the two devices we need. */
static void scan_devices(pluto_iiod_t *c, const char *win, size_t len) {
  static const char tag[] = "<device id=\"";
  for (size_t i = 0; i + sizeof tag < len; i++) {
    if (memcmp(win + i, tag, sizeof tag - 1) != 0) continue;
    const char *id = win + i + sizeof tag - 1;
    const char *idend = memchr(id, '"', len - (size_t)(id - win));
    if (!idend) continue;
    static const char nm[] = "\" name=\"";
    if ((size_t)(win + len - idend) <= sizeof nm || memcmp(idend, nm, sizeof nm - 1) != 0) continue;
    const char *name = idend + sizeof nm - 1;
    const char *nend = memchr(name, '"', len - (size_t)(name - win));
    if (!nend) continue;
    size_t il = (size_t)(idend - id), nl = (size_t)(nend - name);
    if (il >= PLUTO_ID_MAX) continue;
    if (nl == 10 && !memcmp(name, "ad9361-phy", 10)) { memcpy(c->phy, id, il); c->phy[il] = 0; }
    if (nl == 13 && !memcmp(name, "cf-ad9361-lpc", 13)) { memcpy(c->rx, id, il); c->rx[il] = 0; }
  }
}

int pluto_iiod_open(pluto_iiod_t *c, const pluto_io_t *io) {
  memset(c, 0, sizeof *c);
  c->io = *io;
  if (io_send_str(c, "VERSION\r\n") < 0) return PLUTO_ERR_IO;
  char l[64];
  if (read_line(c, l, sizeof l) < 0) return PLUTO_ERR_IO;
  if (!isdigit((unsigned char)l[0])) return PLUTO_ERR_PROTO;
  long n;
  int r = command(c, "PRINT", &n);
  if (r < 0) return r;
  if (n <= 0) return PLUTO_ERR_PROTO;
  /* The xml is ~32 KB: stream it through a window that keeps the last KEEP bytes, so a tag cut by a chunk edge is
   * still seen. A device line is well under KEEP bytes. */
  enum { KEEP = 96, CHUNK = 256 };
  char win[KEEP + CHUNK];
  size_t have = 0, left = (size_t)n;
  while (left) {
    size_t k = left < CHUNK ? left : CHUNK;
    if (c->io.recv(c->io.ctx, win + have, k) != (int)k) return PLUTO_ERR_IO;
    have += k;
    left -= k;
    scan_devices(c, win, have);
    if (have > KEEP) {
      memmove(win, win + have - KEEP, KEEP);
      have = KEEP;
    }
  }
  if (discard(c, 1) < 0) return PLUTO_ERR_IO; /* trailing LF after the xml */
  if (!c->phy[0] || !c->rx[0]) return PLUTO_ERR_NODEV;
  return PLUTO_OK;
}

static int attr_target(char *out, size_t cap, const char *verb, const char *dev, const char *channel, bool output,
                       const char *attr, long len) {
  int n;
  if (channel)
    n = snprintf(out, cap, "%s %s %s %s %s", verb, dev, output ? "OUTPUT" : "INPUT", channel, attr);
  else
    n = snprintf(out, cap, "%s %s %s", verb, dev, attr);
  if (n <= 0 || (size_t)n >= cap) return PLUTO_ERR_RANGE;
  if (len >= 0) {
    int m = snprintf(out + n, cap - (size_t)n, " %ld", len);
    if (m <= 0 || (size_t)(n + m) >= cap) return PLUTO_ERR_RANGE;
  }
  return PLUTO_OK;
}

int pluto_iiod_write_attr(pluto_iiod_t *c, const char *dev, const char *channel, bool output, const char *attr,
                          const char *value) {
  char line[160];
  size_t vl = strlen(value);
  int r = attr_target(line, sizeof line, "WRITE", dev, channel, output, attr, (long)vl);
  if (r < 0) return r;
  /* The command line and the raw value go out back to back; iiod answers once, after the value. */
  size_t ll = strlen(line);
  if (ll + 2 >= sizeof line) return PLUTO_ERR_RANGE;
  line[ll++] = 0x0d;
  line[ll++] = 0x0a;
  if (c->io.send(c->io.ctx, line, ll) != (int)ll) return PLUTO_ERR_IO;
  if (vl && c->io.send(c->io.ctx, value, vl) != (int)vl) return PLUTO_ERR_IO;
  long v;
  r = read_status(c, &v);
  return r < 0 ? r : (int)v;
}

int pluto_iiod_read_attr(pluto_iiod_t *c, const char *dev, const char *channel, bool output, const char *attr,
                         char *out, size_t cap) {
  char line[160];
  int r = attr_target(line, sizeof line, "READ", dev, channel, output, attr, -1);
  if (r < 0) return r;
  long n;
  r = command(c, line, &n);
  if (r < 0) return r;
  if (n < 0) return (int)n;
  size_t keep = (size_t)n < cap ? (size_t)n : (cap ? cap - 1 : 0);
  if (keep && c->io.recv(c->io.ctx, out, keep) != (int)keep) return PLUTO_ERR_IO;
  if (cap) out[keep] = 0;
  if (discard(c, (size_t)n - keep + 1) < 0) return PLUTO_ERR_IO; /* rest of the value + trailing LF */
  return (int)keep;
}

int pluto_iiod_buffer_open(pluto_iiod_t *c, uint32_t samples, uint32_t chan_mask) {
  char line[96];
  snprintf(line, sizeof line, "OPEN %s %lu %08lX", c->rx, (unsigned long)samples, (unsigned long)chan_mask);
  long v;
  int r = command(c, line, &v);
  if (r < 0) return r;
  if (v < 0) return (int)v;
  c->buffer_open = true;
  c->mask_chans = chan_mask;
  return PLUTO_OK;
}

int pluto_iiod_buffer_read(pluto_iiod_t *c, void *dst, size_t bytes) {
  if (!c->buffer_open) return PLUTO_ERR_PROTO;
  char line[96];
  snprintf(line, sizeof line, "READBUF %s %lu", c->rx, (unsigned long)bytes);
  long n;
  int r = command(c, line, &n);
  if (r < 0) return r;
  if (n < 0) return (int)n;
  if ((size_t)n > bytes) return PLUTO_ERR_PROTO;
  char mask[16];
  if (read_line(c, mask, sizeof mask) < 0) return PLUTO_ERR_IO; /* iiod 0.26 sends the active-channel mask line first */
  if (n && c->io.recv(c->io.ctx, dst, (size_t)n) != (int)n) return PLUTO_ERR_IO;
  return (int)n;
}

int pluto_iiod_buffer_close(pluto_iiod_t *c) {
  if (!c->buffer_open) return PLUTO_OK;
  char line[64];
  snprintf(line, sizeof line, "CLOSE %s", c->rx);
  long v;
  c->buffer_open = false;
  int r = command(c, line, &v);
  return r < 0 ? r : (int)v;
}
