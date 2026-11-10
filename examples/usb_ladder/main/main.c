/* USB throughput ladder: raises the AD9361 sample rate step by step and measures how fast the P4 can pull IQ off the Pluto's USB
 * pipe with nothing else running (no conversion, no consumers). If the host cannot keep up, the delivered rate falls below the
 * nominal rate (the Pluto's buffers overflow on its side and the samples are lost). Receive only: never touches TX. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pluto_iiod.h"
#include "pluto_usb.h"

static const char *TAG = "pluto_ladder";

#define SAMPLES_PER_READ 16384u

static const uint32_t kRates[] = {2400000, 3000000, 3500000, 4000000, 4500000, 5000000, 5500000, 6000000, 6500000, 7000000, 8000000};

void app_main(void) {
  ESP_ERROR_CHECK(pluto_usb_init(true));
  ESP_LOGI(TAG, "waiting for a PlutoSDR on the USB host port...");
  while (!pluto_usb_poll()) vTaskDelay(pdMS_TO_TICKS(500));
  vTaskDelay(pdMS_TO_TICKS(300));
  pluto_io_t io;
  int r = pluto_usb_open(&io);
  ESP_LOGI(TAG, "usb open -> %d", r);
  if (r) return;
  static pluto_iiod_t c;
  r = pluto_iiod_open(&c, &io);
  ESP_LOGI(TAG, "iiod open -> %d phy=%s rx=%s", r, c.phy, c.rx);
  if (r) return;
  static int16_t raw[SAMPLES_PER_READ * 2];
  pluto_iiod_write_attr(&c, c.phy, "altvoltage0", true, "frequency", "96100000");
  for (size_t i = 0; i < sizeof kRates / sizeof kRates[0]; ++i) {
    const uint32_t rate = kRates[i];
    char v[32];
    snprintf(v, sizeof v, "%u", (unsigned)rate);
    if (pluto_iiod_write_attr(&c, c.phy, "voltage0", false, "sampling_frequency", v) < 0) {
      ESP_LOGW(TAG, "%u S/s refused by the device", (unsigned)rate);
      continue;
    }
    snprintf(v, sizeof v, "%u", (unsigned)(rate * 8 / 10));
    pluto_iiod_write_attr(&c, c.phy, "voltage0", false, "rf_bandwidth", v);
    char got[32] = {0};
    pluto_iiod_read_attr(&c, c.phy, "voltage0", false, "sampling_frequency", got, sizeof got);
    if (pluto_iiod_buffer_open(&c, SAMPLES_PER_READ, 0x3) < 0) {
      ESP_LOGE(TAG, "buffer open failed at %u", (unsigned)rate);
      break;
    }
    /* Discard the first buffers (the rate change leaves stale data in flight) and measure 4 s. */
    for (int k = 0; k < 4; ++k) (void)pluto_iiod_buffer_read(&c, raw, SAMPLES_PER_READ * 4);
    int64_t t0 = esp_timer_get_time();
    uint64_t bytes = 0;
    unsigned reads = 0, bad = 0;
    int64_t worst_gap = 0, last = t0;
    while (esp_timer_get_time() - t0 < 4000000) {
      int n = pluto_iiod_buffer_read(&c, raw, SAMPLES_PER_READ * 4);
      int64_t now = esp_timer_get_time();
      if (n <= 0) { ++bad; break; }
      if (now - last > worst_gap) worst_gap = now - last;
      last = now;
      bytes += (uint64_t)n;
      ++reads;
    }
    double s = (esp_timer_get_time() - t0) / 1e6;
    double msps = bytes / 4.0 / s / 1e6;
    double nominal = strtod(got, NULL) / 1e6;
    ESP_LOGI(TAG, "LADDER asked=%u device=%s nominal=%.3f MS/s delivered=%.3f MS/s (%.1f%%) %.2f MB/s reads=%u worst_gap_ms=%.1f errors=%u",
             (unsigned)rate, got, nominal, msps, nominal > 0 ? 100.0 * msps / nominal : 0.0, bytes / s / 1e6, reads, worst_gap / 1000.0, bad);
    pluto_iiod_buffer_close(&c);
    if (bad) break;
  }
  /* Leave the Pluto at a low, safe rate. */
  pluto_iiod_write_attr(&c, c.phy, "voltage0", false, "sampling_frequency", "2400000");
  pluto_usb_close();
  ESP_LOGI(TAG, "ladder done");
}
