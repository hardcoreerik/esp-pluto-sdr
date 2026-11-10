#pragma once
/* IQ conversion for PlutoSDR (AD9361) receive data: interleaved int16 I,Q with 12-bit right-aligned samples
 * (full scale +-2048) to the unsigned 8-bit offset-128 IQ that RTL-SDR-style pipelines consume, with optional integer
 * decimation by averaging. Pure functions, no allocation, no dependencies. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t decim;     /* output sample = mean of `decim` input samples (>=1) */
  uint32_t phase;     /* input samples accumulated toward the next output */
  int32_t acc_i, acc_q;
} pluto_iq_conv_t;

void pluto_iq_conv_init(pluto_iq_conv_t *s, uint32_t decim);

/* Converts `n_in` complex samples (4 bytes each: I, Q int16) to u8 IQ pairs; returns the number of output samples
 * written (2 bytes each). `out` must hold n_in/decim + 1 samples. Samples beyond +-2048 clip to 0..255. State carries
 * across calls so block boundaries do not matter. */
size_t pluto_iq_convert(pluto_iq_conv_t *s, const int16_t *in, size_t n_in, uint8_t *out);

/* Smallest integer decimation d such that rate_wanted * d >= pluto_min_rate (AD9361 floor 2.083333 MS/s) and
 * rate_wanted * d <= pluto_max_rate. Returns 0 when none exists. */
uint32_t pluto_iq_pick_decimation(uint32_t wanted_sps, uint32_t min_sps, uint32_t max_sps);

#ifdef __cplusplus
}
#endif
