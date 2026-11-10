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

size_t pluto_iq_convert(pluto_iq_conv_t *s, const int16_t *in, size_t n_in, uint8_t *out) {
  size_t o = 0;
  if (s->decim == 1) {
    for (size_t i = 0; i < n_in; i++) {
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
