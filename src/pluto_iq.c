#include "pluto_iq.h"

void pluto_iq_conv_init(pluto_iq_conv_t *s, uint32_t decim) {
  s->decim = decim ? decim : 1;
  s->phase = 0;
  s->acc_i = s->acc_q = 0;
}

static inline uint8_t to_u8(int32_t v) {
  v = (v >> 4) + 128; /* +-2048 -> +-128 */
  return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
}

__attribute__((optimize("O3"))) size_t pluto_iq_convert(pluto_iq_conv_t *s, const int16_t *in, size_t n_in, uint8_t *out) {
  size_t o = 0;
  if (s->decim == 1) {
    /* Hot path (2.4 MS/s on the P4 reads a 64 KiB block from PSRAM every 6.8 ms): four complex samples per iteration. */
    size_t i = 0;
    const size_t n4 = n_in & ~(size_t)3;
    for (; i < n4; i += 4) {
      const int16_t *p = in + 2 * i;
      const int32_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3], a4 = p[4], a5 = p[5], a6 = p[6], a7 = p[7];
      uint8_t *q = out + 2 * i;
      q[0] = to_u8(a0); q[1] = to_u8(a1); q[2] = to_u8(a2); q[3] = to_u8(a3);
      q[4] = to_u8(a4); q[5] = to_u8(a5); q[6] = to_u8(a6); q[7] = to_u8(a7);
    }
    o = 2 * i;
    for (; i < n_in; i++) {
      out[o++] = to_u8(in[2 * i]);
      out[o++] = to_u8(in[2 * i + 1]);
    }
    return o / 2;
  }
  for (size_t i = 0; i < n_in; i++) {
    s->acc_i += in[2 * i];
    s->acc_q += in[2 * i + 1];
    if (++s->phase == s->decim) {
      out[o++] = to_u8(s->acc_i / (int32_t)s->decim);
      out[o++] = to_u8(s->acc_q / (int32_t)s->decim);
      s->phase = 0;
      s->acc_i = s->acc_q = 0;
    }
  }
  return o / 2;
}

uint32_t pluto_iq_pick_decimation(uint32_t wanted, uint32_t min_sps, uint32_t max_sps) {
  if (!wanted) return 0;
  for (uint32_t d = 1; (uint64_t)wanted * d <= max_sps; d++)
    if ((uint64_t)wanted * d >= min_sps) return d;
  return 0;
}
