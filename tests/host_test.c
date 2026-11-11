#define _POSIX_C_SOURCE 200809L
/* Host test. Without arguments: pure checks of the IQ converter. With a host argument: live receive-only smoke test
 * against a real radio (sets RX rate/LO, reads buffers, never touches TX). Build: see tests/run.sh */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#error "build under WSL/Linux"
#endif
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <unistd.h>

#include "pluto_iiod.h"
#include "pluto_iq.h"
#include "pluto_plan.h"

static int s_send(void *ctx, const void *b, size_t n) { return send(*(int *)ctx, b, n, 0) == (ssize_t)n ? (int)n : -5; }
static int s_recv(void *ctx, void *b, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = recv(*(int *)ctx, (char *)b + got, n - got, 0);
    if (r <= 0) return -5;
    got += (size_t)r;
  }
  return (int)n;
}

static void test_iq(void) {
  int16_t in[8] = {0, 0, 2047, -2048, 4000, -4000, 16, -16};
  uint8_t out[8];
  pluto_iq_conv_t s;
  pluto_iq_conv_init(&s, 1);
  assert(pluto_iq_convert(&s, in, 4, out) == 4);
  assert(out[0] == 128 && out[1] == 128 && out[2] == 255 && out[3] == 0 && out[4] == 255 && out[5] == 0);
  assert(out[6] == 129 && out[7] == 127);
  pluto_iq_conv_init(&s, 2); /* block split must not matter */
  uint8_t a[8], b[8];
  size_t n1 = pluto_iq_convert(&s, in, 3, a), n2 = pluto_iq_convert(&s, in + 6, 1, b);
  assert(n1 == 1 && n2 == 1 && a[0] == 191 /* (0+2047)/2>>4+128 */ && a[1] == 64);
  assert(pluto_iq_pick_decimation(1440000, 2083333, 61440000) == 2);
  assert(pluto_iq_pick_decimation(960000, 2083333, 61440000) == 3);
  assert(pluto_iq_pick_decimation(240000, 2083333, 61440000) == 9);
  assert(pluto_iq_pick_decimation(2048000, 2083333, 61440000) == 2);
  puts("iq: ok");
}


static void test_plan(void) {
  pluto_range_t r;
  assert(pluto_parse_range("[70000000 1 6000000000]", &r) && r.lo == 70000000 && r.hi == 6000000000LL && r.step == 1);
  assert(!pluto_parse_range("garbage", &r) && !r.ok);
  assert(!pluto_parse_range("[5 0 9]", &r)); /* zero step */
  pluto_rate_plan_t p;
  /* 960 kS/s is below the device minimum: converter at 2.88 MS/s, decimate by 3 */
  assert(pluto_plan_rate(960000, 2083333, 61440000, 5500000, true, &p) == 0 && p.out_sps == 960000 && p.ad_sps == 2880000 && p.decim == 3 && !p.clamped);
  assert(pluto_plan_rate(2400000, 2083333, 61440000, 5500000, true, &p) == 0 && p.out_sps == 2400000 && p.ad_sps == 2400000 && p.decim == 1);
  /* above the link ceiling: clamped to it, undecimated */
  assert(pluto_plan_rate(8000000, 2083333, 61440000, 5500000, true, &p) == 0 && p.out_sps == 5500000 && p.decim == 1 && p.clamped);
  /* native 16-bit cannot be decimated up from below the minimum */
  assert(pluto_plan_rate(960000, 2083333, 61440000, 5500000, false, &p) == -2);
  assert(pluto_plan_rate(0, 2083333, 61440000, 5500000, true, &p) == -1);
  assert(pluto_plan_rate(1000000, 2083333, 61440000, 1000000, true, &p) == -1); /* link ceiling below the device minimum */
  pluto_range_t bw = {200000, 1, 56000000, true};
  assert(pluto_auto_bandwidth(2400000, &bw) == 1920000);
  assert(pluto_auto_bandwidth(100000, &bw) == 200000); /* never below the device minimum */
  assert(pluto_auto_bandwidth(100000, NULL) == 200000);
  pluto_range_t g = {-1, 1, 73, true};
  int steps[32];
  size_t n = pluto_build_gain_ladder(&g, steps, 32);
  assert(n >= 2 && n <= 32 && steps[0] == 0 && steps[n - 1] <= 730 && steps[1] > steps[0]);
  puts("plan: ok");
}

/* A scripted transport: the bytes a device would send, to check how the client reacts to a damaged or unexpected block. */
typedef struct {
  const char *data;
  size_t len, pos;
} script_t;
static int sc_send(void *ctx, const void *b, size_t n) {
  (void)ctx;
  (void)b;
  return (int)n;
}
static int sc_recv(void *ctx, void *b, size_t n) {
  script_t *s = ctx;
  if (s->pos + n > s->len) return -5;
  memcpy(b, s->data + s->pos, n);
  s->pos += n;
  return (int)n;
}
static int readbuf(const char *reply, size_t len) {
  script_t s = {reply, len, 0};
  pluto_iiod_t c;
  memset(&c, 0, sizeof c);
  c.io.send = sc_send;
  c.io.recv = sc_recv;
  c.io.ctx = &s;
  c.buffer_open = true;
  c.mask_chans = 3;
  strcpy(c.rx, "iio:device3");
  uint8_t dst[64];
  return pluto_iiod_buffer_read(&c, dst, sizeof dst);
}
static void test_readbuf(void) {
  assert(readbuf("8\n00000003\n12345678", 19) == 8);  /* the requested mask, whole samples */
  assert(readbuf("8\n00000001\n12345678", 19) < 0);   /* a different channel mask than asked for */
  assert(readbuf("6\n00000003\n123456", 17) < 0);     /* not a whole number of samples */
  assert(readbuf("8\n0000zz03\n12345678", 19) < 0);   /* unparseable mask */
  puts("readbuf: ok");
}

static void live(const char *host) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(30431)};
  inet_pton(AF_INET, host, &a.sin_addr);
  struct timeval tv = {10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  assert(connect(fd, (struct sockaddr *)&a, sizeof a) == 0);
  pluto_io_t io = {s_send, s_recv, &fd};
  pluto_iiod_t c;
  int r = pluto_iiod_open(&c, &io);
  printf("open=%d phy=%s rx=%s\n", r, c.phy, c.rx);
  assert(r == 0);
  char v[64];
  assert(pluto_iiod_write_attr(&c, c.phy, "voltage0", false, "sampling_frequency", "2880000") > 0);
  assert(pluto_iiod_write_attr(&c, c.phy, "voltage0", false, "rf_bandwidth", "1300000") > 0);
  assert(pluto_iiod_write_attr(&c, c.phy, "altvoltage0", true, "frequency", "162550000") > 0);
  assert(pluto_iiod_read_attr(&c, c.phy, "voltage0", false, "sampling_frequency", v, sizeof v) > 0);
  printf("rate readback=%s\n", v);
  assert(pluto_iiod_read_attr(&c, c.phy, "altvoltage0", true, "frequency", v, sizeof v) > 0);
  printf("lo readback=%s\n", v);
  assert(pluto_iiod_buffer_open(&c, 16384, 0x3) == 0);
  static int16_t raw[16384 * 2];
  static uint8_t u8[16384 * 2];
  pluto_iq_conv_t conv;
  pluto_iq_conv_init(&conv, 2); /* 2.88 MS/s -> 1.44 MS/s */
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  size_t bytes = 0, out_samples = 0;
  int minv = 255, maxv = 0;
  for (int i = 0; i < 40; i++) {
    int n = pluto_iiod_buffer_read(&c, raw, sizeof raw);
    assert(n > 0 && n % 4 == 0);
    bytes += (size_t)n;
    size_t o = pluto_iq_convert(&conv, raw, (size_t)n / 4, u8);
    out_samples += o;
    for (size_t k = 0; k < o * 2; k++) { if (u8[k] < minv) minv = u8[k]; if (u8[k] > maxv) maxv = u8[k]; }
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
  printf("live: %zu bytes in %.2f s = %.2f MB/s (needed %.2f), %zu u8 samples, u8 range %d..%d\n", bytes, s,
         bytes / s / 1e6, 2.88 * 4, out_samples, minv, maxv);
  assert(pluto_iiod_buffer_close(&c) == 0);
  close(fd);
  puts("live: ok");
}

int main(int argc, char **argv) {
  test_iq();
  test_plan();
  test_readbuf();
  if (argc > 1) live(argv[1]);
  return 0;
}
