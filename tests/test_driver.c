#include "esp_pluto_sdr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
  const char *rx;
  size_t len, pos;
  char tx[4096];
  size_t used;
} wire;
static int rd(void *p, void *b, size_t n) {
  wire *w = p;
  if (w->pos == w->len)
    return -1;
  if (n > 1)
    n = 1;
  memcpy(b, w->rx + w->pos, n);
  w->pos += n;
  return (int)n;
}
static int wr(void *p, const void *b, size_t n) {
  wire *w = p;
  if (n > 2)
    n = 2;
  memcpy(w->tx + w->used, b, n);
  w->used += n;
  w->tx[w->used] = 0;
  return (int)n;
}
static pluto_client client(wire *w) {
  pluto_client c;
  pluto_init(&c, (pluto_io){w, rd, wr});
  return c;
}
int main(void) {
  wire w = {.rx = "0\n4\n00000003\n\x01\x02\x03\x04", .len = 17};
  w.len = 2 + 2 + 9 + 4;
  pluto_client c = client(&w);
  assert(pluto_rx_open(&c, "iio:device1", 8, 3) == 0);
  unsigned char b[8];
  size_t got = 0;
  assert(pluto_rx_read(&c, b, 4, &got) == 0 && got == 4 && b[0] == 1 &&
         b[3] == 4);
  assert(strstr(w.tx, "OPEN iio:device1 8 00000003\r\n"));
  assert(strstr(w.tx, "READBUF iio:device1 4\r\n"));
  wire bad = {.rx = "0\n9\n", .len = 4};
  c = client(&bad);
  assert(!pluto_rx_open(&c, "iio:device1", 8, 3));
  assert(pluto_rx_read(&c, b, 4, &got) == PLUTO_PROTOCOL);
  assert(c.faulted);
  wire neg = {.rx = "-22\n", .len = 4};
  c = client(&neg);
  assert(pluto_rx_open(&c, "iio:device1", 8, 3) == -22);
  assert(!c.streaming);
  wire inject = {0};
  c = client(&inject);
  assert(pluto_rx_open(&c, "bad\nWRITE", 8, 3) == PLUTO_INVALID);
  assert(inject.used == 0);
  wire chunks = {.rx = "0\n4\n00000003\nabcd4\nefgh", .len = 25};
  chunks.len = strlen(chunks.rx);
  c = client(&chunks);
  assert(!pluto_rx_open(&c, "rx", 8, 3));
  assert(!pluto_rx_read(&c, b, 8, &got) && got == 8 &&
         !memcmp(b, "abcdefgh", 8));
  wire mask = {.rx = "0\n4\n0000000f\nabcd", .len = 17};
  c = client(&mask);
  assert(!pluto_rx_open(&c, "rx", 8, 3));
  assert(pluto_rx_read(&c, b, 4, &got) == PLUTO_PROTOCOL);
  wire ctx = {.rx = "4\n<xml\n", .len = 7};
  c = client(&ctx);
  char xml[16];
  assert(!pluto_context(&c, xml, sizeof(xml), &got));
  assert(got == 4 && !memcmp(xml, "<xml", 4));
  wire tune = {.rx = "2\n0\n", .len = 4};
  c = client(&tune);
  assert(!pluto_attr_write(&c, "ad9361-phy", "OUTPUT", "altvoltage0",
                           "frequency", "42"));
  assert(!strcmp(tune.tx,
                 "WRITE ad9361-phy OUTPUT altvoltage0 frequency 2\r\n42"));
  wire closewire = {.rx = "0\n0\n", .len = 4};
  c = client(&closewire);
  assert(!pluto_rx_open(&c, "rx", 8, 3));
  assert(!pluto_rx_close(&c));
  assert(!c.streaming);
  wire truncated = {.rx = "0\n4\n00000003\nab", .len = 15};
  c = client(&truncated);
  assert(!pluto_rx_open(&c, "rx", 8, 3));
  assert(pluto_rx_read(&c, b, 4, &got) == PLUTO_IO);
  assert(c.faulted);
  wire huge = {.rx = "1000\n", .len = 5};
  c = client(&huge);
  assert(pluto_context(&c, xml, sizeof xml, &got) == PLUTO_PROTOCOL);
  puts("driver protocol checks passed");
  return 0;
}
