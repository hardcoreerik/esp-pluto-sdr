#pragma once
/* Pure planning helpers for the PlutoSDR (AD9361): parsing the ranges the device reports, choosing the converter rate and decimation for a
 * wanted output rate, the analog bandwidth policy and the gain ladder. No I/O, no allocation, host-testable. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  long long lo, step, hi;
  bool ok;
} pluto_range_t;

/* Parses an iio "[lo step hi]" range string. */
bool pluto_parse_range(const char *s, pluto_range_t *out);

typedef struct {
  uint32_t out_sps;  /* rate the consumer receives */
  uint32_t ad_sps;   /* rate the AD9361 runs at (out_sps * decim) */
  uint32_t decim;    /* integer decimation (>= 1) */
  bool clamped;      /* the wanted rate was above what the device or the link allows and was lowered */
} pluto_rate_plan_t;

/* Plans a wanted output rate. `dev_min`/`dev_max` are the device's converter rate range, `link_max` the highest converter rate the host
 * link is allowed to carry (a measured number, not the device's), `allow_decim` false for streams that cannot be decimated (native 16-bit).
 * `max_decim` caps the integer decimation (0 = no cap): a boxcar average over many samples is a poor filter, so very low output rates are refused
 * instead of being produced badly. Returns 0 and fills `plan`, or a negative value: -1 invalid argument, -2 the rate is below the device minimum
 * and cannot be decimated up within the cap. */
int pluto_plan_rate(uint32_t wanted_sps, uint32_t dev_min, uint32_t dev_max, uint32_t link_max, bool allow_decim, uint32_t max_decim,
                    pluto_rate_plan_t *plan);

/* Analog RF bandwidth for an output rate: 80% of it, kept inside the device's range when known (the AD9361 minimum is 200 kHz). */
uint32_t pluto_auto_bandwidth(uint32_t out_sps, const pluto_range_t *bw_range);

/* Builds a manual-gain ladder in tenths of a dB, at most `max_steps` entries spread over the device's own span (negative values dropped).
 * Returns the number of steps written. */
size_t pluto_build_gain_ladder(const pluto_range_t *gain_db, int *steps_db10, size_t max_steps);

#ifdef __cplusplus
}
#endif
