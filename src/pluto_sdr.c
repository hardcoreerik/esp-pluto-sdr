#include "pluto_sdr.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pluto_iiod.h"
#include "pluto_iq.h"
#include "pluto_plan.h"
#include "pluto_usb.h"

static const char *TAG = "pluto_sdr";

enum { XO_NOMINAL_HZ = 40000000 };
enum { RATE_SETTLE_US = 400000, GAIN_POLL_US = 1000000, WINDOW_US = 750000, SCAN_US = 1000000, RETRY_US = 1000000 };

typedef struct {
  uint32_t idx, bytes, epoch, rate;
  uint64_t hz;
  int64_t time_us;
} item_t;

struct pluto_sdr {
  pluto_sdr_config_t cfg;
  SemaphoreHandle_t lock; /* protects req, caps, info, gain, tune result and the freq/rate the controls read back */
  TaskHandle_t worker;

  /* requests from the control calls (guarded by lock) */
  struct {
    uint64_t freq;
    bool freq_dirty;
    uint32_t rate;
    bool rate_dirty;
    uint32_t bw;
    bool bw_dirty;
    int gain_kind; /* 0 none, 1 auto, 2 manual */
    int gain_db10;
    int32_t ppm_milli;
    bool ppm_dirty;
    bool trk_dirty, trk_rf, trk_bb, trk_quad;
  } req;
  pluto_sdr_controls_t ctl;
  uint64_t cur_freq, tune_hz;
  int tune_err;
  uint32_t cur_rate;
  pluto_sdr_caps_t caps;
  pluto_sdr_info_t info;
  pluto_sdr_gain_t gain;

  atomic_int state;
  atomic_bool want_run, quit, exited, present, session_active;

  /* streaming; the worker owns the iiod client and the buffer */
  pluto_iiod_t iiod;
  bool buffer_open;
  pluto_rate_plan_t plan;
  pluto_range_t freq_range, rate_range, bw_range, gain_range, xo_range;

  /* hand-off to read(): conv, epoch and decim are guarded by conv_lock */
  SemaphoreHandle_t conv_lock;
  pluto_iq_conv_t conv;
  uint32_t epoch;
  uint32_t decim;
  atomic_uint generation;
  int16_t *raw[16];
  size_t raw_bytes;
  QueueHandle_t filled, freeq;
  uint32_t nbuf;
  uint32_t sequence; /* read() only */

  portMUX_TYPE mlock;
  pluto_sdr_metrics_t m;
};

static void emit(struct pluto_sdr *h, pluto_sdr_event_t ev) {
  if (h->cfg.event_cb) h->cfg.event_cb(h, ev, h->cfg.event_ctx);
}

const char *pluto_sdr_err_to_name(int e) {
  switch (e) {
    case PLUTO_SDR_OK: return "OK";
    case PLUTO_SDR_ERR_INVALID_ARG: return "INVALID_ARG";
    case PLUTO_SDR_ERR_NOT_CONNECTED: return "NOT_CONNECTED";
    case PLUTO_SDR_ERR_TIMEOUT: return "TIMEOUT";
    case PLUTO_SDR_ERR_BAD_FREQ: return "BAD_FREQ";
    case PLUTO_SDR_ERR_BAD_RATE: return "BAD_RATE";
    case PLUTO_SDR_ERR_BAD_STATE: return "BAD_STATE";
    case PLUTO_SDR_ERR_NO_MEM: return "NO_MEM";
    case PLUTO_SDR_ERR_IO: return "IO";
    case PLUTO_SDR_ERR_UNSUPPORTED: return "UNSUPPORTED";
    case PLUTO_SDR_ERR_BAD_GAIN: return "BAD_GAIN";
    default: return "UNKNOWN";
  }
}

const char *pluto_sdr_state_to_name(pluto_sdr_state_t s) {
  switch (s) {
    case PLUTO_SDR_STATE_IDLE: return "IDLE";
    case PLUTO_SDR_STATE_PRESENT: return "PRESENT";
    case PLUTO_SDR_STATE_OPENING: return "OPENING";
    case PLUTO_SDR_STATE_STREAMING: return "STREAMING";
    case PLUTO_SDR_STATE_ERROR: return "ERROR";
    default: return "UNKNOWN";
  }
}

const char *pluto_sdr_get_version_string(void) { return PLUTO_SDR_VERSION_STRING; }

void pluto_sdr_config_default(pluto_sdr_config_t *c) {
  memset(c, 0, sizeof *c);
  c->install_usb_host = false;
  c->format = PLUTO_SDR_FORMAT_CU8;
  c->read_samples = 16384;
  c->buffers = 4;
  c->link_max_sps = 5500000;
  c->max_decimation = 20;
  c->worker_core = 1;
  c->worker_priority = 7;
}

/* ---- device access (worker task only) ---------------------------------------------------------------------------------------------- */

static bool wr_num(struct pluto_sdr *h, const char *ch, bool out, const char *attr, long long v) {
  char b[24];
  snprintf(b, sizeof b, "%lld", v);
  return pluto_iiod_write_attr(&h->iiod, h->iiod.phy, ch, out, attr, b) >= 0;
}

static bool rd_range(struct pluto_sdr *h, const char *ch, bool out, const char *attr, pluto_range_t *r) {
  char v[64] = {0};
  if (pluto_iiod_read_attr(&h->iiod, h->iiod.phy, ch, out, attr, v, sizeof v) < 0) {
    r->ok = false;
    return false;
  }
  return pluto_parse_range(v, r);
}

static void close_buffer(struct pluto_sdr *h) {
  if (h->buffer_open) pluto_iiod_buffer_close(&h->iiod);
  h->buffer_open = false;
}

static void refresh_gain(struct pluto_sdr *h) {
  char v[40] = {0}, m[40] = {0};
  double db = 0;
  bool manual = false;
  if (pluto_iiod_read_attr(&h->iiod, h->iiod.phy, "voltage0", false, "hardwaregain", v, sizeof v) > 0) db = atof(v);
  if (pluto_iiod_read_attr(&h->iiod, h->iiod.phy, "voltage0", false, "gain_control_mode", m, sizeof m) > 0) manual = strncmp(m, "manual", 6) == 0;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  h->gain.manual = manual;
  h->gain.gain_db10 = (int)(db * 10.0 + (db < 0 ? -0.5 : 0.5));
  xSemaphoreGive(h->lock);
}

static long long rd_ll(struct pluto_sdr *h, const char *dev_ch, bool out, const char *attr, bool *ok) {
  char v[48] = {0};
  const char *ch = (dev_ch && dev_ch[0]) ? dev_ch : NULL;
  int r = pluto_iiod_read_attr(&h->iiod, h->iiod.phy, ch, out, attr, v, sizeof v);
  if (ok) *ok = r > 0;
  return r > 0 ? atoll(v) : 0;
}

static double rd_dbl(struct pluto_sdr *h, const char *ch, bool out, const char *attr, bool *ok) {
  char v[48] = {0};
  int r = pluto_iiod_read_attr(&h->iiod, h->iiod.phy, ch, out, attr, v, sizeof v);
  if (ok) *ok = r > 0;
  return r > 0 ? atof(v) : 0.0;
}

/* One attribute per call, in rotation: a burst of reads between two buffer reads could starve the USB pipe, so the readback cache is filled
 * one slow step at a time. */
static void refresh_step(struct pluto_sdr *h, int step) {
  bool ok = false;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  pluto_sdr_controls_t c = h->ctl;
  xSemaphoreGive(h->lock);
  switch (step % 8) {
    case 0: { long long v = rd_ll(h, "altvoltage0", true, "frequency", &ok); if (ok) c.center_actual_hz = (uint64_t)v; break; }
    case 1: { long long v = rd_ll(h, "voltage0", false, "rf_bandwidth", &ok); if (ok) c.bandwidth_hz = (uint32_t)v; break; }
    case 2: { long long v = rd_ll(h, "voltage0", false, "sampling_frequency", &ok); if (ok) c.rate_converter_sps = (uint32_t)v; break; }
    case 3: { double v = rd_dbl(h, "voltage0", false, "rssi", &ok); if (ok) c.rssi_db10 = (int)(v * 10.0 + (v < 0 ? -0.5 : 0.5)); break; }
    case 4: {
      long long v = rd_ll(h, NULL, false, "xo_correction", &ok);
      if (ok) {
        c.xo_hz = (uint32_t)v;
        c.ppm_milli = (int32_t)((v - XO_NOMINAL_HZ) * 25);
      }
      break;
    }
    case 5: { long long v = rd_ll(h, "voltage0", false, "rf_dc_offset_tracking_en", &ok); if (ok) c.dc_tracking_rf = v != 0; break; }
    case 6: { long long v = rd_ll(h, "voltage0", false, "bb_dc_offset_tracking_en", &ok); if (ok) c.dc_tracking_bb = v != 0; break; }
    case 7: { long long v = rd_ll(h, "voltage0", false, "quadrature_tracking_en", &ok); if (ok) c.quadrature_tracking = v != 0; break; }
  }
  xSemaphoreTake(h->lock, portMAX_DELAY);
  c.valid = true;
  c.center_requested_hz = h->cur_freq;
  c.center_error_hz = (int64_t)c.center_actual_hz - (int64_t)h->cur_freq;
  c.rate_requested_sps = h->req.rate;
  c.rate_out_sps = h->cur_rate;
  c.decimation = h->decim;
  c.gain_manual = h->gain.manual;
  c.gain_db10 = h->gain.gain_db10;
  h->ctl = c;
  xSemaphoreGive(h->lock);
}

static void fill_caps(struct pluto_sdr *h) {
  pluto_sdr_caps_t c;
  memset(&c, 0, sizeof c);
  c.valid = true;
  c.tune_min_hz = h->freq_range.ok ? (uint64_t)h->freq_range.lo : 0;
  c.tune_max_hz = h->freq_range.ok ? (uint64_t)h->freq_range.hi : 0;
  c.rate_min_sps = h->rate_range.ok ? (uint32_t)h->rate_range.lo : 2083334;
  c.rate_max_sps = h->rate_range.ok ? (uint32_t)(h->rate_range.hi > 0xFFFFFFFFLL ? 0xFFFFFFFFLL : h->rate_range.hi) : 61440000;
  c.rate_link_max_sps = h->cfg.link_max_sps;
  c.out_rate_min_sps = (h->cfg.format == PLUTO_SDR_FORMAT_CU8 && h->cfg.max_decimation) ? (c.rate_min_sps + h->cfg.max_decimation - 1) / h->cfg.max_decimation : c.rate_min_sps;
  c.bw_min_hz = h->bw_range.ok ? (uint32_t)h->bw_range.lo : 200000;
  c.bw_max_hz = h->bw_range.ok ? (uint32_t)h->bw_range.hi : 56000000;
  if (h->gain_range.ok) {
    c.gain_min_db10 = (int)(h->gain_range.lo * 10);
    c.gain_max_db10 = (int)(h->gain_range.hi * 10);
  }
  c.ppm_min_milli = -200000;
  c.ppm_max_milli = 200000;
  if (h->xo_range.ok) {
    c.ppm_min_milli = (int32_t)((h->xo_range.lo - XO_NOMINAL_HZ) * 25);
    c.ppm_max_milli = (int32_t)((h->xo_range.hi - XO_NOMINAL_HZ) * 25);
  }
  c.adc_bits = 12;
  c.rx_channels = 2;
  c.gain_auto = true;
  c.hardware_tx = true;
  c.tx_enabled = false;
  pluto_sdr_gain_t g;
  memset(&g, 0, sizeof g);
  g.step_count = (uint8_t)pluto_build_gain_ladder(&h->gain_range, g.steps_db10, 32);
  xSemaphoreTake(h->lock, portMAX_DELAY);
  h->caps = c;
  h->gain.step_count = g.step_count;
  memcpy(h->gain.steps_db10, g.steps_db10, sizeof g.steps_db10);
  xSemaphoreGive(h->lock);
}

/* Applies the converter rate for the wanted OUTPUT rate. The buffer is closed and reopened; blocks already queued belong to the old rate and
 * are discarded by read() through the epoch. */
static bool apply_rate(struct pluto_sdr *h, uint32_t wanted, uint32_t bw_req) {
  close_buffer(h);
  pluto_rate_plan_t p;
  const bool allow_decim = h->cfg.format == PLUTO_SDR_FORMAT_CU8;
  const uint32_t dmin = h->rate_range.ok ? (uint32_t)h->rate_range.lo : 2083334;
  const uint32_t dmax = h->rate_range.ok ? (uint32_t)(h->rate_range.hi > 0xFFFFFFFFLL ? 0xFFFFFFFFLL : h->rate_range.hi) : 61440000;
  if (pluto_plan_rate(wanted, dmin, dmax, h->cfg.link_max_sps, allow_decim, h->cfg.max_decimation, &p) != 0) return false;
  if (!wr_num(h, "voltage0", false, "sampling_frequency", p.ad_sps)) return false;
  (void)wr_num(h, "voltage0", false, "rf_bandwidth", bw_req ? bw_req : pluto_auto_bandwidth(p.out_sps, &h->bw_range));
  char v[32] = {0};
  if (pluto_iiod_read_attr(&h->iiod, h->iiod.phy, "voltage0", false, "sampling_frequency", v, sizeof v) > 0) {
    const long long got = atoll(v);
    /* The AD9361 rounds to a few Hz (2.4 MS/s reads back as 2,399,999). Within 2 Hz the nominal rate stays, as a dongle's does. */
    if (got > 0 && llabs(got - (long long)p.ad_sps) > 2) {
      ESP_LOGW(TAG, "asked %u S/s, device runs %lld", (unsigned)p.ad_sps, got);
      p.ad_sps = (uint32_t)got;
      p.out_sps = p.ad_sps / p.decim;
    }
  }
  xSemaphoreTake(h->conv_lock, portMAX_DELAY);
  pluto_iq_conv_init(&h->conv, p.decim);
  h->decim = p.decim;
  ++h->epoch;
  xSemaphoreGive(h->conv_lock);
  atomic_fetch_add(&h->generation, 1);
  h->plan = p;
  if (pluto_iiod_buffer_open(&h->iiod, h->cfg.read_samples, 0x3) < 0) return false;
  h->buffer_open = true;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  h->cur_rate = p.out_sps;
  xSemaphoreGive(h->lock);
  ESP_LOGI(TAG, "RATE out=%u ad9361=%u decim=%u%s", (unsigned)p.out_sps, (unsigned)p.ad_sps, (unsigned)p.decim, p.clamped ? " (clamped)" : "");
  return true;
}

static void apply_requests(struct pluto_sdr *h, int64_t now, uint32_t *pending_rate, int64_t *rate_changed_at, uint32_t *applied_req_rate,
                           uint32_t *applied_bw) {
  uint64_t freq = 0;
  uint32_t rate = 0, bw = 0;
  bool fdirty, rdirty, bdirty;
  int gkind, gval;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  int32_t ppm = h->req.ppm_milli;
  bool ppm_dirty = h->req.ppm_dirty, trk_dirty = h->req.trk_dirty, t_rf = h->req.trk_rf, t_bb = h->req.trk_bb, t_q = h->req.trk_quad;
  h->req.ppm_dirty = h->req.trk_dirty = false;
  fdirty = h->req.freq_dirty;
  freq = h->req.freq;
  h->req.freq_dirty = false;
  rdirty = h->req.rate_dirty;
  rate = h->req.rate;
  h->req.rate_dirty = false;
  bdirty = h->req.bw_dirty;
  bw = h->req.bw;
  h->req.bw_dirty = false;
  gkind = h->req.gain_kind;
  gval = h->req.gain_db10;
  h->req.gain_kind = 0;
  xSemaphoreGive(h->lock);

  if (fdirty) {
    int err = PLUTO_SDR_OK;
    if (h->freq_range.ok && (freq < (uint64_t)h->freq_range.lo || freq > (uint64_t)h->freq_range.hi)) err = PLUTO_SDR_ERR_BAD_FREQ;
    else if (!wr_num(h, "altvoltage0", true, "frequency", (long long)freq)) err = PLUTO_SDR_ERR_BAD_FREQ;
    long long actual = 0;
    bool rb = false;
    if (!err) actual = rd_ll(h, "altvoltage0", true, "frequency", &rb); /* what the synthesizer really settled on */
    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->tune_hz = freq;
    h->tune_err = err;
    if (!err) {
      h->cur_freq = freq;
      if (rb) h->ctl.center_actual_hz = (uint64_t)actual;
    }
    xSemaphoreGive(h->lock);
    ESP_LOGI(TAG, "TUNE %llu -> %s", (unsigned long long)freq, err ? pluto_sdr_err_to_name(err) : "ok");
  }
  if (ppm_dirty) {
    long long xo = XO_NOMINAL_HZ + (long long)((double)ppm * 0.04 + (ppm < 0 ? -0.5 : 0.5));
    if (h->xo_range.ok && (xo < h->xo_range.lo || xo > h->xo_range.hi)) ESP_LOGW(TAG, "frequency correction %d milli-ppm is outside the device span", (int)ppm);
    else if (!wr_num(h, NULL, false, "xo_correction", xo)) ESP_LOGW(TAG, "xo_correction %lld refused", xo);
    else ESP_LOGI(TAG, "XO correction %d milli-ppm (xo %lld Hz)", (int)ppm, xo);
  }
  if (trk_dirty) {
    (void)wr_num(h, "voltage0", false, "rf_dc_offset_tracking_en", t_rf ? 1 : 0);
    (void)wr_num(h, "voltage0", false, "bb_dc_offset_tracking_en", t_bb ? 1 : 0);
    (void)wr_num(h, "voltage0", false, "quadrature_tracking_en", t_q ? 1 : 0);
  }
  if (gkind == 1) {
    (void)pluto_iiod_write_attr(&h->iiod, h->iiod.phy, "voltage0", false, "gain_control_mode", "slow_attack");
    refresh_gain(h);
  } else if (gkind == 2) {
    long long db = gval / 10;
    if (h->gain_range.ok) db = db < h->gain_range.lo ? h->gain_range.lo : (db > h->gain_range.hi ? h->gain_range.hi : db);
    if (pluto_iiod_write_attr(&h->iiod, h->iiod.phy, "voltage0", false, "gain_control_mode", "manual") >= 0) (void)wr_num(h, "voltage0", false, "hardwaregain", db);
    refresh_gain(h);
  }
  if (bdirty && bw != *applied_bw) {
    (void)wr_num(h, "voltage0", false, "rf_bandwidth", bw ? bw : pluto_auto_bandwidth(h->plan.out_sps, &h->bw_range));
    *applied_bw = bw;
  }
  /* A rate change is a buffer close and reopen: wait until the request has been steady for a moment (callers often ask twice in quick
   * succession), the rule the RTL-SDR driver's loop applies too. */
  if (rdirty && rate != *pending_rate) {
    *pending_rate = rate;
    *rate_changed_at = now;
  }
  if (*pending_rate && *pending_rate != *applied_req_rate && now - *rate_changed_at > RATE_SETTLE_US) {
    if (apply_rate(h, *pending_rate, *applied_bw)) {
      *applied_req_rate = *pending_rate;
      emit(h, PLUTO_SDR_EVENT_RATE_CHANGED);
    } else {
      ESP_LOGE(TAG, "rate change to %u failed", (unsigned)*pending_rate);
      *applied_req_rate = *pending_rate; /* do not retry the same value in a loop; the session ends if no buffer is open */
    }
  }
}

/* One attach: claim the pipe, learn the device, configure it and stream until stop, an error or unplug. */
static void run_session(struct pluto_sdr *h) {
  pluto_io_t io;
  atomic_store(&h->state, PLUTO_SDR_STATE_OPENING);
  if (pluto_usb_open(&io) != 0) {
    ESP_LOGE(TAG, "libiio USB pipe did not open");
    atomic_store(&h->state, PLUTO_SDR_STATE_ERROR);
    return;
  }
  if (pluto_iiod_open(&h->iiod, &io) != 0) {
    ESP_LOGE(TAG, "iiod handshake failed");
    atomic_store(&h->state, PLUTO_SDR_STATE_ERROR);
    return;
  }
  pluto_sdr_info_t info;
  memset(&info, 0, sizeof info);
  bool hs = false;
  pluto_usb_describe(info.manufacturer, sizeof info.manufacturer, info.product, sizeof info.product, info.serial, sizeof info.serial, &hs);
  info.vid = PLUTO_USB_VID;
  info.pid = PLUTO_USB_PID;
  info.high_speed = hs;
  rd_range(h, "altvoltage0", true, "frequency_available", &h->freq_range);
  rd_range(h, "voltage0", false, "sampling_frequency_available", &h->rate_range);
  rd_range(h, "voltage0", false, "rf_bandwidth_available", &h->bw_range);
  rd_range(h, "voltage0", false, "hardwaregain_available", &h->gain_range);
  {
    char xv[64] = {0};
    if (pluto_iiod_read_attr(&h->iiod, h->iiod.phy, NULL, false, "xo_correction_available", xv, sizeof xv) > 0) pluto_parse_range(xv, &h->xo_range);
    else h->xo_range.ok = false;
  }
  ESP_LOGI(TAG, "ranges: tune %lld..%lld Hz, rate %lld..%lld S/s, gain %lld..%lld dB", h->freq_range.lo, h->freq_range.hi, h->rate_range.lo,
           h->rate_range.hi, h->gain_range.lo, h->gain_range.hi);
  xSemaphoreTake(h->lock, portMAX_DELAY);
  h->info = info;
  xSemaphoreGive(h->lock);
  fill_caps(h);
  refresh_gain(h);

  xSemaphoreTake(h->lock, portMAX_DELAY);
  uint64_t start_freq = h->req.freq ? h->req.freq : 100000000ULL;
  uint32_t start_rate = h->req.rate ? h->req.rate : 960000;
  uint32_t start_bw = h->req.bw;
  h->req.freq_dirty = h->req.rate_dirty = h->req.bw_dirty = false;
  xSemaphoreGive(h->lock);
  if (h->freq_range.ok && (start_freq < (uint64_t)h->freq_range.lo || start_freq > (uint64_t)h->freq_range.hi)) start_freq = (uint64_t)h->freq_range.lo;
  if (!wr_num(h, "altvoltage0", true, "frequency", (long long)start_freq)) ESP_LOGW(TAG, "start frequency %llu refused", (unsigned long long)start_freq);
  xSemaphoreTake(h->lock, portMAX_DELAY);
  h->cur_freq = start_freq;
  xSemaphoreGive(h->lock);
  bool ok = apply_rate(h, start_rate, start_bw);
  uint32_t applied_req_rate = start_rate, pending_rate = start_rate, applied_bw = start_bw;
  int64_t rate_changed_at = 0, last_gain = esp_timer_get_time(), window_start = last_gain, last_step = last_gain;
  int step_no = 0;
  uint64_t win_in = 0, win_out_base = 0;
  if (ok) {
    xSemaphoreTake(h->lock, portMAX_DELAY);
    win_out_base = 0;
    xSemaphoreGive(h->lock);
    atomic_store(&h->state, PLUTO_SDR_STATE_STREAMING);
    atomic_store(&h->session_active, true);
    taskENTER_CRITICAL(&h->mlock);
    h->m.sessions++;
    win_out_base = h->m.samples_delivered;
    taskEXIT_CRITICAL(&h->mlock);
    emit(h, PLUTO_SDR_EVENT_STREAM_STARTED);
  }
  while (ok && atomic_load(&h->want_run) && !atomic_load(&h->quit) && !pluto_usb_gone()) {
    const int64_t now = esp_timer_get_time();
    apply_requests(h, now, &pending_rate, &rate_changed_at, &applied_req_rate, &applied_bw);
    if (!h->buffer_open) break;
    if (now - last_gain > GAIN_POLL_US) {
      last_gain = now;
      refresh_gain(h);
    }
    if (now - last_step > 120000) {
      last_step = now;
      refresh_step(h, step_no++);
    }
    uint32_t idx = 0;
    const bool have = xQueueReceive(h->freeq, &idx, 0) == pdTRUE;
    const int n = pluto_iiod_buffer_read(&h->iiod, have ? h->raw[idx] : h->raw[h->nbuf], h->raw_bytes);
    if (n <= 0) {
      ESP_LOGW(TAG, "buffer read -> %d, ending the session", n);
      if (have) xQueueSend(h->freeq, &idx, 0);
      taskENTER_CRITICAL(&h->mlock);
      h->m.usb_errors++;
      taskEXIT_CRITICAL(&h->mlock);
      ok = false;
      atomic_store(&h->state, PLUTO_SDR_STATE_ERROR);
      break;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    const uint64_t hz = h->cur_freq;
    xSemaphoreGive(h->lock);
    const uint32_t samples = (uint32_t)n / 4 / (h->cfg.format == PLUTO_SDR_FORMAT_CU8 ? h->decim : 1);
    if (have) {
      const item_t it = {idx, (uint32_t)n, h->epoch, h->plan.out_sps, hz, esp_timer_get_time()}; /* epoch: only this task writes it */
      xQueueSend(h->filled, &it, 0);
    }
    taskENTER_CRITICAL(&h->mlock);
    h->m.samples_received += samples;
    if (!have) h->m.source_drops++;
    taskEXIT_CRITICAL(&h->mlock);
    win_in += samples;
    if (now - window_start >= WINDOW_US) {
      const uint64_t us = (uint64_t)(now - window_start);
      taskENTER_CRITICAL(&h->mlock);
      h->m.effective_in_sps = (uint32_t)(win_in * 1000000ULL / us);
      h->m.effective_out_sps = (uint32_t)((h->m.samples_delivered - win_out_base) * 1000000ULL / us);
      win_out_base = h->m.samples_delivered;
      taskEXIT_CRITICAL(&h->mlock);
      win_in = 0;
      window_start = now;
    }
  }
  close_buffer(h);
  xSemaphoreTake(h->conv_lock, portMAX_DELAY);
  ++h->epoch; /* nothing queued belongs to a stream that no longer exists */
  xSemaphoreGive(h->conv_lock);
  atomic_fetch_add(&h->generation, 1);
  atomic_store(&h->session_active, false);
  if (atomic_load(&h->state) == PLUTO_SDR_STATE_STREAMING || atomic_load(&h->state) == PLUTO_SDR_STATE_OPENING)
    atomic_store(&h->state, ok ? PLUTO_SDR_STATE_PRESENT : PLUTO_SDR_STATE_ERROR);
  emit(h, PLUTO_SDR_EVENT_STREAM_STOPPED);
}

static void worker(void *arg) {
  struct pluto_sdr *h = arg;
  int64_t last_scan = 0, retry_at = 0;
  while (!atomic_load(&h->quit)) {
    const int64_t now = esp_timer_get_time();
    if (!atomic_load(&h->present)) {
      if (now - last_scan > SCAN_US) {
        last_scan = now;
        if (pluto_usb_poll()) {
          atomic_store(&h->present, true);
          atomic_store(&h->state, PLUTO_SDR_STATE_PRESENT);
          emit(h, PLUTO_SDR_EVENT_ATTACHED);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (pluto_usb_gone()) {
      pluto_usb_close();
      atomic_store(&h->present, false);
      atomic_store(&h->state, PLUTO_SDR_STATE_IDLE);
      emit(h, PLUTO_SDR_EVENT_DETACHED);
      continue;
    }
    if (!atomic_load(&h->want_run) || now < retry_at) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(300)); /* a freshly attached device is still enumerating */
    run_session(h);
    /* After a session the USB device is released (stop) or reopened from scratch (error / unplug): the next scan finds it again. */
    const bool failed = atomic_load(&h->state) == PLUTO_SDR_STATE_ERROR;
    const bool unplugged = pluto_usb_gone();
    pluto_usb_close();
    atomic_store(&h->present, false);
    if (unplugged) {
      atomic_store(&h->state, PLUTO_SDR_STATE_IDLE);
      emit(h, PLUTO_SDR_EVENT_DETACHED);
    } else if (!failed) {
      atomic_store(&h->state, PLUTO_SDR_STATE_IDLE);
    }
    if (failed) retry_at = esp_timer_get_time() + RETRY_US;
    last_scan = 0;
  }
  atomic_store(&h->exited, true);
  vTaskDelete(NULL);
}

/* ---- public API ------------------------------------------------------------------------------------------------------------------- */

int pluto_sdr_install(const pluto_sdr_config_t *cfg, pluto_sdr_handle_t *out) {
  if (!cfg || !out || cfg->read_samples < 1024 || cfg->buffers < 2 || cfg->buffers > 15) return PLUTO_SDR_ERR_INVALID_ARG;
  struct pluto_sdr *h = calloc(1, sizeof *h);
  if (!h) return PLUTO_SDR_ERR_NO_MEM;
  h->cfg = *cfg;
  h->mlock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
  h->nbuf = cfg->buffers;
  h->raw_bytes = (size_t)cfg->read_samples * 4;
  h->epoch = 1;
  h->decim = 1;
  h->lock = xSemaphoreCreateMutex();
  h->conv_lock = xSemaphoreCreateMutex();
  h->filled = xQueueCreate(h->nbuf, sizeof(item_t));
  h->freeq = xQueueCreate(h->nbuf, sizeof(uint32_t));
  bool ok = h->lock && h->conv_lock && h->filled && h->freeq;
  for (uint32_t b = 0; ok && b <= h->nbuf; ++b) { /* the extra block is scratch for reads that find no free buffer */
    h->raw[b] = heap_caps_malloc(h->raw_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!h->raw[b]) h->raw[b] = heap_caps_malloc(h->raw_bytes, MALLOC_CAP_8BIT);
    ok = ok && h->raw[b];
    if (ok && b < h->nbuf) xQueueSend(h->freeq, &b, 0);
  }
  if (ok && pluto_usb_init(cfg->install_usb_host) != 0) ok = false;
  if (!ok) {
    for (uint32_t b = 0; b <= h->nbuf; ++b) heap_caps_free(h->raw[b]);
    if (h->filled) vQueueDelete(h->filled);
    if (h->freeq) vQueueDelete(h->freeq);
    if (h->lock) vSemaphoreDelete(h->lock);
    if (h->conv_lock) vSemaphoreDelete(h->conv_lock);
    free(h);
    return PLUTO_SDR_ERR_NO_MEM;
  }
  atomic_store(&h->state, PLUTO_SDR_STATE_IDLE);
  atomic_store(&h->exited, false);
  if (xTaskCreatePinnedToCore(worker, "pluto_sdr", 6144, h, cfg->worker_priority, &h->worker, cfg->worker_core > 1 ? tskNO_AFFINITY : cfg->worker_core) !=
      pdPASS) {
    ESP_LOGE(TAG, "worker task could not be created");
    return PLUTO_SDR_ERR_NO_MEM; /* the handle is leaked on purpose: a half-built driver is not torn down under a possible live task */
  }
  *out = h;
  return PLUTO_SDR_OK;
}

int pluto_sdr_uninstall(pluto_sdr_handle_t h) {
  if (!h) return PLUTO_SDR_ERR_INVALID_ARG;
  atomic_store(&h->want_run, false);
  atomic_store(&h->quit, true);
  for (int i = 0; i < 300 && !atomic_load(&h->exited); ++i) vTaskDelay(pdMS_TO_TICKS(10));
  if (!atomic_load(&h->exited)) return PLUTO_SDR_ERR_TIMEOUT; /* the worker is still running: nothing is freed */
  for (uint32_t b = 0; b <= h->nbuf; ++b) heap_caps_free(h->raw[b]);
  vQueueDelete(h->filled);
  vQueueDelete(h->freeq);
  vSemaphoreDelete(h->lock);
  vSemaphoreDelete(h->conv_lock);
  free(h);
  return PLUTO_SDR_OK;
}

int pluto_sdr_start(pluto_sdr_handle_t h) {
  if (!h) return PLUTO_SDR_ERR_INVALID_ARG;
  atomic_store(&h->want_run, true);
  return PLUTO_SDR_OK;
}

int pluto_sdr_stop(pluto_sdr_handle_t h, uint32_t timeout_ms) {
  if (!h) return PLUTO_SDR_ERR_INVALID_ARG;
  atomic_store(&h->want_run, false);
  const int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
  while (esp_timer_get_time() < end) {
    pluto_sdr_state_t s = (pluto_sdr_state_t)atomic_load(&h->state);
    if (s != PLUTO_SDR_STATE_STREAMING && s != PLUTO_SDR_STATE_OPENING && !atomic_load(&h->session_active)) return PLUTO_SDR_OK;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return PLUTO_SDR_ERR_TIMEOUT;
}

pluto_sdr_state_t pluto_sdr_get_state(pluto_sdr_handle_t h) { return h ? (pluto_sdr_state_t)atomic_load(&h->state) : PLUTO_SDR_STATE_IDLE; }
bool pluto_sdr_device_present(pluto_sdr_handle_t h) { return h && atomic_load(&h->present); }

int pluto_sdr_get_capabilities(pluto_sdr_handle_t h, pluto_sdr_caps_t *out) {
  if (!h || !out) return PLUTO_SDR_ERR_INVALID_ARG;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  *out = h->caps;
  xSemaphoreGive(h->lock);
  return PLUTO_SDR_OK;
}

int pluto_sdr_get_device_info(pluto_sdr_handle_t h, pluto_sdr_info_t *out) {
  if (!h || !out) return PLUTO_SDR_ERR_INVALID_ARG;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  *out = h->info;
  xSemaphoreGive(h->lock);
  return PLUTO_SDR_OK;
}

int pluto_sdr_get_metrics(pluto_sdr_handle_t h, pluto_sdr_metrics_t *out) {
  if (!h || !out) return PLUTO_SDR_ERR_INVALID_ARG;
  taskENTER_CRITICAL(&h->mlock);
  *out = h->m;
  taskEXIT_CRITICAL(&h->mlock);
  return PLUTO_SDR_OK;
}

int pluto_sdr_set_center_freq(pluto_sdr_handle_t h, uint64_t hz) {
  if (!h || hz == 0) return PLUTO_SDR_ERR_INVALID_ARG;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  int err = PLUTO_SDR_OK;
  if (h->caps.valid && (hz < h->caps.tune_min_hz || hz > h->caps.tune_max_hz)) err = PLUTO_SDR_ERR_BAD_FREQ;
  else {
    h->req.freq = hz;
    h->req.freq_dirty = true;
  }
  xSemaphoreGive(h->lock);
  return err;
}

uint64_t pluto_sdr_get_center_freq(pluto_sdr_handle_t h) {
  if (!h) return 0;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  uint64_t v = h->cur_freq ? h->cur_freq : h->req.freq;
  xSemaphoreGive(h->lock);
  return v;
}

int pluto_sdr_get_tune_result(pluto_sdr_handle_t h, uint64_t *hz, int *err) {
  if (!h) return PLUTO_SDR_ERR_INVALID_ARG;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  if (hz) *hz = h->tune_hz;
  if (err) *err = h->tune_err;
  xSemaphoreGive(h->lock);
  return PLUTO_SDR_OK;
}

int pluto_sdr_set_sample_rate(pluto_sdr_handle_t h, uint32_t out_sps) {
  if (!h || out_sps == 0) return PLUTO_SDR_ERR_INVALID_ARG;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  int err = PLUTO_SDR_OK;
  if (h->caps.valid && h->cfg.format == PLUTO_SDR_FORMAT_CS16 && out_sps < h->caps.rate_min_sps) err = PLUTO_SDR_ERR_BAD_RATE; /* no decimation in CS16 */
  else {
    h->req.rate = out_sps;
    h->req.rate_dirty = true;
  }
  xSemaphoreGive(h->lock);
  return err;
}

uint32_t pluto_sdr_get_sample_rate(pluto_sdr_handle_t h) {
  if (!h) return 0;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  uint32_t v = atomic_load(&h->session_active) ? h->cur_rate : 0;
  xSemaphoreGive(h->lock);
  return v;
}

int pluto_sdr_set_rf_bandwidth(pluto_sdr_handle_t h, uint32_t hz) {
  if (!h) return PLUTO_SDR_ERR_INVALID_ARG;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  int err = PLUTO_SDR_OK;
  if (hz && h->caps.valid && (hz < h->caps.bw_min_hz || hz > h->caps.bw_max_hz)) err = PLUTO_SDR_ERR_INVALID_ARG;
  else {
    h->req.bw = hz;
    h->req.bw_dirty = true;
  }
  xSemaphoreGive(h->lock);
  return err;
}

int pluto_sdr_set_gain_auto(pluto_sdr_handle_t h) {
  if (!h) return PLUTO_SDR_ERR_INVALID_ARG;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  h->req.gain_kind = 1;
  xSemaphoreGive(h->lock);
  return PLUTO_SDR_OK;
}

int pluto_sdr_set_gain_manual(pluto_sdr_handle_t h, int gain_db10) {
  if (!h) return PLUTO_SDR_ERR_INVALID_ARG;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  int err = PLUTO_SDR_OK;
  if (h->caps.valid && (gain_db10 < h->caps.gain_min_db10 || gain_db10 > h->caps.gain_max_db10)) err = PLUTO_SDR_ERR_BAD_GAIN;
  else {
    h->req.gain_kind = 2;
    h->req.gain_db10 = gain_db10;
  }
  xSemaphoreGive(h->lock);
  return err;
}

int pluto_sdr_get_gain(pluto_sdr_handle_t h, pluto_sdr_gain_t *out) {
  if (!h || !out) return PLUTO_SDR_ERR_INVALID_ARG;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  *out = h->gain;
  xSemaphoreGive(h->lock);
  return PLUTO_SDR_OK;
}

int pluto_sdr_get_controls(pluto_sdr_handle_t h, pluto_sdr_controls_t *out) {
  if (!h || !out) return PLUTO_SDR_ERR_INVALID_ARG;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  *out = h->ctl;
  out->center_requested_hz = h->cur_freq ? h->cur_freq : h->req.freq;
  out->rate_requested_sps = h->req.rate;
  xSemaphoreGive(h->lock);
  return PLUTO_SDR_OK;
}

int pluto_sdr_set_freq_correction(pluto_sdr_handle_t h, int32_t ppm_milli) {
  if (!h) return PLUTO_SDR_ERR_INVALID_ARG;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  int err = PLUTO_SDR_OK;
  if (h->caps.valid && (ppm_milli < h->caps.ppm_min_milli || ppm_milli > h->caps.ppm_max_milli)) err = PLUTO_SDR_ERR_INVALID_ARG;
  else {
    h->req.ppm_milli = ppm_milli;
    h->req.ppm_dirty = true;
  }
  xSemaphoreGive(h->lock);
  return err;
}

int pluto_sdr_set_tracking(pluto_sdr_handle_t h, bool dc_rf, bool dc_bb, bool quadrature) {
  if (!h) return PLUTO_SDR_ERR_INVALID_ARG;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  h->req.trk_rf = dc_rf;
  h->req.trk_bb = dc_bb;
  h->req.trk_quad = quadrature;
  h->req.trk_dirty = true;
  xSemaphoreGive(h->lock);
  return PLUTO_SDR_OK;
}

int pluto_sdr_quantize_sample_rate(pluto_sdr_handle_t h, uint32_t wanted, uint32_t *applied) {
  if (!h || !applied || wanted == 0) return PLUTO_SDR_ERR_INVALID_ARG;
  xSemaphoreTake(h->lock, portMAX_DELAY);
  const bool valid = h->caps.valid;
  const uint32_t dmin = h->caps.rate_min_sps, dmax = h->caps.rate_max_sps;
  xSemaphoreGive(h->lock);
  if (!valid) return PLUTO_SDR_ERR_NOT_CONNECTED;
  pluto_rate_plan_t p;
  if (pluto_plan_rate(wanted, dmin, dmax, h->cfg.link_max_sps, h->cfg.format == PLUTO_SDR_FORMAT_CU8, h->cfg.max_decimation, &p) != 0) return PLUTO_SDR_ERR_BAD_RATE;
  *applied = p.out_sps;
  return PLUTO_SDR_OK;
}

size_t pluto_sdr_max_block_bytes(pluto_sdr_handle_t h) {
  if (!h) return 0;
  return h->cfg.format == PLUTO_SDR_FORMAT_CU8 ? (size_t)h->cfg.read_samples * 2 + 8 : h->raw_bytes;
}

int pluto_sdr_read(pluto_sdr_handle_t h, void *dst, size_t dst_cap, uint32_t timeout_ms, pluto_sdr_block_t *meta) {
  if (!h || !dst) return PLUTO_SDR_ERR_INVALID_ARG;
  if (dst_cap < pluto_sdr_max_block_bytes(h)) return PLUTO_SDR_ERR_INVALID_ARG;
  const int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
  for (;;) {
    if (!atomic_load(&h->session_active) && uxQueueMessagesWaiting(h->filled) == 0) {
      if (esp_timer_get_time() >= end) return PLUTO_SDR_ERR_NOT_CONNECTED;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    int64_t left_us = end - esp_timer_get_time();
    item_t it;
    if (xQueueReceive(h->filled, &it, left_us > 0 ? pdMS_TO_TICKS((left_us + 999) / 1000) : 0) != pdTRUE) return PLUTO_SDR_ERR_TIMEOUT;
    xSemaphoreTake(h->conv_lock, portMAX_DELAY);
    if (it.epoch != h->epoch) { /* read under an earlier rate or session: never converted with the current decimation */
      xSemaphoreGive(h->conv_lock);
      taskENTER_CRITICAL(&h->mlock);
      h->m.stale_blocks++;
      taskEXIT_CRITICAL(&h->mlock);
      xQueueSend(h->freeq, &it.idx, 0);
      continue;
    }
    size_t bytes;
    uint32_t samples;
    if (h->cfg.format == PLUTO_SDR_FORMAT_CU8) {
      samples = (uint32_t)pluto_iq_convert(&h->conv, h->raw[it.idx], it.bytes / 4, dst);
      bytes = (size_t)samples * 2;
    } else {
      memcpy(dst, h->raw[it.idx], it.bytes);
      samples = it.bytes / 4;
      bytes = it.bytes;
    }
    xSemaphoreGive(h->conv_lock);
    xQueueSend(h->freeq, &it.idx, 0);
    taskENTER_CRITICAL(&h->mlock);
    h->m.samples_delivered += samples;
    taskEXIT_CRITICAL(&h->mlock);
    if (meta) {
      meta->format = h->cfg.format;
      meta->sample_rate = it.rate;
      meta->center_hz = it.hz;
      meta->generation = atomic_load(&h->generation);
      meta->sequence = h->sequence;
      meta->time_us = it.time_us;
      meta->samples = samples;
    }
    ++h->sequence;
    if (bytes == 0) continue; /* a block that decimated to nothing (partial group) */
    return (int)bytes;
  }
}
