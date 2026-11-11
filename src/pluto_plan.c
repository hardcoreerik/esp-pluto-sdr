#include "pluto_plan.h"

#include <stdio.h>

#include "pluto_iq.h"

bool pluto_parse_range(const char *s, pluto_range_t *out) {
  if (!out) return false;
  out->lo = out->step = out->hi = 0;
  out->ok = false;
  if (!s) return false;
  long long a, b, c;
  if (sscanf(s, "[%lld %lld %lld]", &a, &b, &c) != 3) return false;
  if (b <= 0 || c < a) return false;
  out->lo = a;
  out->step = b;
  out->hi = c;
  out->ok = true;
  return true;
}

int pluto_plan_rate(uint32_t wanted, uint32_t dev_min, uint32_t dev_max, uint32_t link_max, bool allow_decim, pluto_rate_plan_t *plan) {
  if (!plan || wanted == 0 || dev_min == 0 || dev_max < dev_min) return -1;
  uint32_t ceiling = link_max && link_max < dev_max ? link_max : dev_max;
  if (ceiling < dev_min) return -1;
  plan->clamped = false;
  if (wanted > ceiling) { /* above what the device or the link can carry: run at the highest allowed rate, undecimated */
    plan->out_sps = plan->ad_sps = ceiling;
    plan->decim = 1;
    plan->clamped = true;
    return 0;
  }
  if (wanted >= dev_min) {
    plan->out_sps = plan->ad_sps = wanted;
    plan->decim = 1;
    return 0;
  }
  if (!allow_decim) return -2;
  uint32_t d = pluto_iq_pick_decimation(wanted, dev_min, ceiling);
  if (d == 0) return -2;
  plan->out_sps = wanted;
  plan->ad_sps = wanted * d;
  plan->decim = d;
  return 0;
}

uint32_t pluto_auto_bandwidth(uint32_t out_sps, const pluto_range_t *r) {
  uint64_t bw = (uint64_t)out_sps * 8 / 10;
  if (r && r->ok) {
    if (bw < (uint64_t)r->lo) bw = (uint64_t)r->lo;
    if (bw > (uint64_t)r->hi) bw = (uint64_t)r->hi;
  } else if (bw < 200000) {
    bw = 200000;
  }
  return (uint32_t)bw;
}

size_t pluto_build_gain_ladder(const pluto_range_t *g, int *steps, size_t max_steps) {
  if (!g || !g->ok || !steps || max_steps == 0) return 0;
  long long lo = g->lo < 0 ? 0 : g->lo;
  long long span = g->hi - lo;
  if (span < 0) return 0;
  long long stride = (span + (long long)max_steps - 2) / ((long long)max_steps - 1 > 0 ? (long long)max_steps - 1 : 1);
  if (stride < g->step) stride = g->step;
  if (stride < 1) stride = 1;
  size_t n = 0;
  for (long long v = lo; v <= g->hi && n < max_steps; v += stride) steps[n++] = (int)(v * 10);
  return n;
}
