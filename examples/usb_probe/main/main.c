/* Receive-only probe: finds a PlutoSDR on the P4's USB host port, opens its libiio pipe through the pluto_usb transport,
 * runs the iiod client over it and measures IQ MB/s. Never touches TX. Output goes to the console UART. */
#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pluto_iiod.h"
#include "pluto_usb.h"

static const char *TAG = "pluto_probe";

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
  char v[48];
  pluto_iiod_write_attr(&c, c.phy, "voltage0", false, "sampling_frequency", "2880000");
  pluto_iiod_write_attr(&c, c.phy, "voltage0", false, "rf_bandwidth", "1300000");
  pluto_iiod_write_attr(&c, c.phy, "altvoltage0", true, "frequency", "162550000");
  pluto_iiod_read_attr(&c, c.phy, "voltage0", false, "sampling_frequency", v, sizeof v);
  ESP_LOGI(TAG, "rate readback %s", v);
  r = pluto_iiod_buffer_open(&c, 16384, 0x3);
  ESP_LOGI(TAG, "buffer open -> %d", r);
  if (r) return;
  static int16_t raw[16384 * 2];
  int64_t t0 = esp_timer_get_time();
  size_t bytes = 0;
  int loops = 0;
  while (esp_timer_get_time() - t0 < 5000000) {
    int n = pluto_iiod_buffer_read(&c, raw, sizeof raw);
    if (n <= 0) { ESP_LOGE(TAG, "buffer read -> %d", n); break; }
    bytes += (size_t)n;
    loops++;
  }
  double s = (esp_timer_get_time() - t0) / 1e6;
  ESP_LOGI(TAG, "USB: %u bytes in %.2f s = %.2f MB/s (2.88 MS/s needs 11.52 MB/s), %d buffers", (unsigned)bytes, s,
           bytes / s / 1e6, loops);
  pluto_iiod_buffer_close(&c);
  pluto_usb_close();
  ESP_LOGI(TAG, "done");
}
