/* Standalone test of the esp-pluto-sdr driver on an M5Stack Tab5 (ESP32-P4 rev v1.3). It uses only the driver's public API (pluto_sdr.h) and one board
 * file that switches the Tab5's USB-A 5 V rail. Nothing here knows about OrcSDR. Receive only: no transmit attribute is ever written.
 *
 * What it does, in order, logging every check as PASS or FAIL and ending with one TAB5_RESULT line:
 *   1. powers the USB-A port, installs the driver (it installs the USB host itself), starts it and waits for the Pluto to stream;
 *   2. checks the capabilities the device reports;
 *   3. runs at 0.96 and 2.4 MS/s (8-bit output) and compares the delivered rate with the nominal one;
 *   4. retunes across five bands and compares the device's own LO readback;
 *   5. exercises gain, RF bandwidth, reference correction (ppm) and the offset trackers against the driver's readback;
 *   6. checks that bad requests are refused;
 *   7. reinstalls in native 16-bit mode and walks the sample rate up to find where the host or link stops keeping up. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pluto_sdr.h"
#include "tab5_power.h"

static const char *TAG = "pluto_tab5";
static int g_checks, g_fails;

static void check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void check(bool ok, const char *fmt, ...) {
  char b[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  ++g_checks;
  if (!ok) ++g_fails;
  printf("%s %s\n", ok ? "PASS" : "FAIL", b);
}

static bool wait_streaming(pluto_sdr_handle_t h, int seconds) {
  for (int i = 0; i < seconds * 10; ++i) {
    if (pluto_sdr_get_state(h) == PLUTO_SDR_STATE_STREAMING) return true;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return false;
}

static bool wait_rate(pluto_sdr_handle_t h, uint32_t sps, int seconds) {
  for (int i = 0; i < seconds * 10; ++i) {
    if (pluto_sdr_get_sample_rate(h) == sps) return true;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return false;
}

/* Reads for `ms` and returns the delivered samples per second. */
static double measure(pluto_sdr_handle_t h, uint8_t *buf, size_t cap, int ms, uint32_t *nominal_out) {
  pluto_sdr_block_t meta;
  uint64_t samples = 0;
  int64_t t0 = esp_timer_get_time(), end = t0 + (int64_t)ms * 1000;
  uint32_t nominal = 0;
  while (esp_timer_get_time() < end) {
    int n = pluto_sdr_read(h, buf, cap, 200, &meta);
    if (n > 0) {
      samples += meta.samples;
      nominal = meta.sample_rate;
    }
  }
  if (nominal_out) *nominal_out = nominal;
  double s = (esp_timer_get_time() - t0) / 1e6;
  return s > 0 ? (double)samples / s : 0;
}

static bool wait_center(pluto_sdr_handle_t h, uint64_t hz, int seconds) {
  pluto_sdr_controls_t c;
  for (int i = 0; i < seconds * 10; ++i) {
    pluto_sdr_get_controls(h, &c);
    if (c.valid && c.center_actual_hz == hz) return true;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return false;
}

static void phase_cu8(pluto_sdr_handle_t h) {
  pluto_sdr_caps_t caps;
  pluto_sdr_info_t info;
  pluto_sdr_get_capabilities(h, &caps);
  pluto_sdr_get_device_info(h, &info);
  printf("device: %s serial=%s usb_high_speed=%d\n", info.product, info.serial, info.high_speed);
  check(caps.valid && caps.tune_min_hz == 70000000 && caps.tune_max_hz == 6000000000ULL, "capabilities read from the device (tune %llu..%llu Hz)",
        (unsigned long long)caps.tune_min_hz, (unsigned long long)caps.tune_max_hz);
  check(caps.adc_bits == 12 && caps.rx_channels == 2, "12-bit converter, 2 RX channels");
  check(caps.hardware_tx && !caps.tx_enabled, "hardware TX present, never enabled by the driver");
  check(info.high_speed, "the USB link is high speed");

  const size_t cap = pluto_sdr_max_block_bytes(h);
  uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = heap_caps_malloc(cap, MALLOC_CAP_8BIT);
  check(buf != NULL, "read buffer allocated (%u bytes)", (unsigned)cap);
  if (!buf) return;

  const uint32_t rates[] = {960000, 2400000};
  for (int i = 0; i < 2; ++i) {
    pluto_sdr_metrics_t m0, m1;
    check(pluto_sdr_set_sample_rate(h, rates[i]) == PLUTO_SDR_OK, "set_sample_rate(%u) accepted", (unsigned)rates[i]);
    check(wait_rate(h, rates[i], 8), "output rate becomes %u (reads %u)", (unsigned)rates[i], (unsigned)pluto_sdr_get_sample_rate(h));
    (void)measure(h, buf, cap, 1500, NULL); /* settle: the first blocks after a rate change are discarded */
    pluto_sdr_get_metrics(h, &m0);
    uint32_t nominal = 0;
    double got = measure(h, buf, cap, 4000, &nominal);
    pluto_sdr_get_metrics(h, &m1);
    printf("MEASURE rate=%u delivered=%.0f S/s (%.2f%%) source_drops=%u stale=%u usb_errors=%u\n", (unsigned)rates[i], got, 100.0 * got / rates[i],
           (unsigned)(m1.source_drops - m0.source_drops), (unsigned)m1.stale_blocks, (unsigned)m1.usb_errors);
    check(got > rates[i] * 0.98 && got < rates[i] * 1.02, "delivered rate within 2%% of %u (%.0f)", (unsigned)rates[i], got);
    check(m1.source_drops - m0.source_drops == 0, "no blocks dropped at %u S/s (%u)", (unsigned)rates[i], (unsigned)(m1.source_drops - m0.source_drops));
  }

  const uint64_t freqs[] = {96100000ULL, 162550000ULL, 433920000ULL, 1090000000ULL, 2400000000ULL};
  for (int i = 0; i < 5; ++i) {
    check(pluto_sdr_set_center_freq(h, freqs[i]) == PLUTO_SDR_OK, "set_center_freq(%llu) accepted", (unsigned long long)freqs[i]);
    check(wait_center(h, freqs[i], 6), "the device's LO reads %llu Hz", (unsigned long long)freqs[i]);
  }
  check(pluto_sdr_set_center_freq(h, 50000000ULL) == PLUTO_SDR_ERR_BAD_FREQ, "50 MHz is refused (below the device span)");
  check(pluto_sdr_set_center_freq(h, 7000000000ULL) == PLUTO_SDR_ERR_BAD_FREQ, "7 GHz is refused (above the device span)");
  check(pluto_sdr_set_center_freq(h, 0) == PLUTO_SDR_ERR_INVALID_ARG, "0 Hz is refused");

  pluto_sdr_controls_t c;
  pluto_sdr_set_gain_manual(h, 400);
  vTaskDelay(pdMS_TO_TICKS(2500));
  pluto_sdr_get_controls(h, &c);
  check(c.gain_manual && c.gain_db10 >= 390 && c.gain_db10 <= 410, "manual gain 40 dB (reads %d tenths, manual=%d)", c.gain_db10, c.gain_manual);
  check(pluto_sdr_set_gain_manual(h, 900) == PLUTO_SDR_ERR_BAD_GAIN, "90 dB gain is refused");
  pluto_sdr_set_gain_auto(h);
  vTaskDelay(pdMS_TO_TICKS(2500));
  pluto_sdr_get_controls(h, &c);
  check(!c.gain_manual, "automatic gain restored (manual=%d)", c.gain_manual);

  pluto_sdr_set_rf_bandwidth(h, 1000000);
  vTaskDelay(pdMS_TO_TICKS(2500));
  pluto_sdr_get_controls(h, &c);
  check(c.bandwidth_hz >= 950000 && c.bandwidth_hz <= 1050000, "RF bandwidth 1 MHz (reads %u)", (unsigned)c.bandwidth_hz);
  pluto_sdr_set_rf_bandwidth(h, 0);

  const int32_t ppms[] = {5000, -5000, 0};
  for (int i = 0; i < 3; ++i) {
    pluto_sdr_set_freq_correction(h, ppms[i]);
    vTaskDelay(pdMS_TO_TICKS(2500));
    pluto_sdr_get_controls(h, &c);
    check(c.ppm_milli >= ppms[i] - 25 && c.ppm_milli <= ppms[i] + 25, "reference correction %+d milli-ppm (reads %d, xo %u Hz)", (int)ppms[i], (int)c.ppm_milli,
          (unsigned)c.xo_hz);
  }
  check(pluto_sdr_set_freq_correction(h, 900000) == PLUTO_SDR_ERR_INVALID_ARG, "900 ppm is refused");

  pluto_sdr_set_tracking(h, false, false, false);
  vTaskDelay(pdMS_TO_TICKS(2500));
  pluto_sdr_get_controls(h, &c);
  check(!c.dc_tracking_rf && !c.dc_tracking_bb && !c.quadrature_tracking, "all three offset trackers off");
  pluto_sdr_set_tracking(h, true, true, true);
  vTaskDelay(pdMS_TO_TICKS(2500));
  pluto_sdr_get_controls(h, &c);
  check(c.dc_tracking_rf && c.dc_tracking_bb && c.quadrature_tracking, "all three offset trackers on again");

  pluto_sdr_metrics_t m;
  pluto_sdr_get_metrics(h, &m);
  check(m.usb_errors == 0, "no USB errors in the 8-bit phase (%u)", (unsigned)m.usb_errors);
  heap_caps_free(buf);
}

static void phase_cs16_ladder(void) {
  pluto_sdr_config_t cfg;
  pluto_sdr_config_default(&cfg);
  cfg.install_usb_host = false; /* already installed by the first driver instance */
  cfg.format = PLUTO_SDR_FORMAT_CS16;
  cfg.link_max_sps = 8000000; /* let the ladder go past the default ceiling so the real limit shows */
  pluto_sdr_handle_t h = NULL;
  int r = pluto_sdr_install(&cfg, &h);
  check(r == PLUTO_SDR_OK, "reinstall in native 16-bit mode (%s)", pluto_sdr_err_to_name(r));
  if (r != PLUTO_SDR_OK) return;
  pluto_sdr_set_sample_rate(h, 2400000);
  pluto_sdr_set_center_freq(h, 96100000ULL);
  pluto_sdr_start(h);
  check(wait_streaming(h, 90), "streaming in 16-bit mode");
  const size_t cap = pluto_sdr_max_block_bytes(h);
  uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    pluto_sdr_uninstall(h);
    return;
  }
  const uint32_t ladder[] = {2400000, 3000000, 4000000, 5000000, 5500000, 6000000, 7000000};
  for (size_t i = 0; i < sizeof ladder / sizeof ladder[0]; ++i) {
    pluto_sdr_set_sample_rate(h, ladder[i]);
    wait_rate(h, ladder[i], 10);
    (void)measure(h, buf, cap, 1500, NULL);
    pluto_sdr_metrics_t m0, m1;
    pluto_sdr_get_metrics(h, &m0);
    uint32_t nominal = 0;
    double got = measure(h, buf, cap, 4000, &nominal);
    pluto_sdr_get_metrics(h, &m1);
    printf("LADDER asked=%u nominal=%u delivered=%.0f S/s (%.1f%%) %.2f MB/s source_drops=%u\n", (unsigned)ladder[i], (unsigned)nominal, got,
           nominal ? 100.0 * got / nominal : 0.0, got * 4 / 1e6, (unsigned)(m1.source_drops - m0.source_drops));
  }
  pluto_sdr_set_sample_rate(h, 2400000);
  vTaskDelay(pdMS_TO_TICKS(1500));
  heap_caps_free(buf);
  pluto_sdr_stop(h, 3000);
  pluto_sdr_uninstall(h);
}

void app_main(void) {
  vTaskDelay(pdMS_TO_TICKS(5000)); /* the console is USB: give the host time to reattach after a reset before the first line is printed */
  printf("\n=== esp-pluto-sdr %s on the M5Stack Tab5 ===\n", pluto_sdr_get_version_string());
  /* A reset of the P4 does not reset the expander that holds the USB-A rail, so after a reflash the Pluto is still powered and its USB side can be
   * left stuck mid-stream. Cut the rail for a moment first so the Pluto always boots fresh (about 30 s) and enumerates cleanly. */
  check(tab5_usb_power(false), "USB-A 5 V rail switched off");
  vTaskDelay(pdMS_TO_TICKS(2000));
  check(tab5_usb_power(true), "USB-A 5 V rail switched on");
  vTaskDelay(pdMS_TO_TICKS(500));

  pluto_sdr_config_t cfg;
  pluto_sdr_config_default(&cfg);
  cfg.install_usb_host = true;
  cfg.format = PLUTO_SDR_FORMAT_CU8;
  pluto_sdr_handle_t h = NULL;
  int r = pluto_sdr_install(&cfg, &h);
  check(r == PLUTO_SDR_OK, "driver installed (%s)", pluto_sdr_err_to_name(r));
  if (r == PLUTO_SDR_OK) {
    pluto_sdr_set_sample_rate(h, 960000);
    pluto_sdr_set_center_freq(h, 96100000ULL);
    pluto_sdr_start(h);
    printf("waiting for the Pluto (it takes about 30 s to boot after its power comes on)...\n");
    bool up = wait_streaming(h, 120);
    check(up, "the Pluto is streaming (state %s)", pluto_sdr_state_to_name(pluto_sdr_get_state(h)));
    if (up) {
      phase_cu8(h);
      pluto_sdr_stop(h, 3000);
      pluto_sdr_uninstall(h);
      vTaskDelay(pdMS_TO_TICKS(1000));
      phase_cs16_ladder();
    }
  }
  printf("TAB5_RESULT checks=%d fails=%d %s\n", g_checks, g_fails, g_fails ? "FAIL" : "PASS");
  for (;;) vTaskDelay(pdMS_TO_TICKS(60000));
}
