/* Receive-only probe: finds a PlutoSDR on the P4's USB host port, prints its descriptors, opens a libiio USB pipe and
 * speaks the iiod text protocol over the bulk endpoints (the same client the TCP path uses), then measures IQ MB/s.
 * Never touches TX. Output goes to the console UART. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pluto_iiod.h"
#include "usb/usb_host.h"

static const char *TAG = "pluto_usb";

enum { IIO_USB_CLASS = 0xFF, IIO_USB_SUBCLASS = 0xEB };
/* libiio usb backend vendor requests (interface recipient, host to device), from memory of libiio usb.c:
 * wValue = pipe index, wIndex = interface. The descriptors printed below show whether the layout matches. */
enum { CMD_RESET_PIPES = 0, CMD_OPEN_PIPE = 1 };

static struct {
  usb_host_client_handle_t client;
  usb_device_handle_t dev;
  volatile uint8_t addr;
  uint8_t if_num, ep_in, ep_out;
  SemaphoreHandle_t done;
  usb_transfer_t *xfer;
  uint8_t *rx;
  size_t rx_len, rx_pos;
} g;

#define RX_CHUNK 16384

static void xfer_cb(usb_transfer_t *t) { xSemaphoreGive((SemaphoreHandle_t)t->context); }

static int run_transfer(uint8_t ep, size_t bytes, uint32_t timeout_ms) {
  g.xfer->device_handle = g.dev;
  g.xfer->bEndpointAddress = ep;
  g.xfer->num_bytes = (int)bytes;
  g.xfer->callback = xfer_cb;
  g.xfer->context = g.done;
  g.xfer->timeout_ms = timeout_ms;
  esp_err_t e = ep == 0 ? usb_host_transfer_submit_control(g.client, g.xfer) : usb_host_transfer_submit(g.xfer);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "submit ep %02x len %u -> %s", ep, (unsigned)bytes, esp_err_to_name(e));
    return -5;
  }
  if (xSemaphoreTake(g.done, pdMS_TO_TICKS(timeout_ms + 500)) != pdTRUE) return -110;
  return g.xfer->status == USB_TRANSFER_STATUS_COMPLETED ? g.xfer->actual_num_bytes : -(int)(100 + g.xfer->status);
}

static int ctrl_out(uint8_t req, uint16_t value, uint16_t index) {
  usb_setup_packet_t *s = (usb_setup_packet_t *)g.xfer->data_buffer;
  s->bmRequestType = 0x41; /* vendor, interface, host-to-device */
  s->bRequest = req;
  s->wValue = value;
  s->wIndex = index;
  s->wLength = 0;
  return run_transfer(0, sizeof *s, 2000);
}

static int u_send(void *ctx, const void *b, size_t n) {
  (void)ctx;
  const uint8_t *p = b;
  size_t left = n;
  while (left) {
    size_t k = left < 4096 ? left : 4096;
    memcpy(g.xfer->data_buffer, p, k);
    int r = run_transfer(g.ep_out, k, 3000);
    if (r < 0) return r;
    p += k;
    left -= k;
  }
  return (int)n;
}

static int u_recv(void *ctx, void *b, size_t n) {
  (void)ctx;
  uint8_t *p = b;
  size_t got = 0;
  while (got < n) {
    if (g.rx_pos == g.rx_len) {
      int r = run_transfer(g.ep_in, RX_CHUNK, 5000);
      if (r < 0) return r;
      memcpy(g.rx, g.xfer->data_buffer, (size_t)r);
      g.rx_len = (size_t)r;
      g.rx_pos = 0;
      continue; /* zero-length packets just loop */
    }
    size_t k = g.rx_len - g.rx_pos;
    if (k > n - got) k = n - got;
    memcpy(p + got, g.rx + g.rx_pos, k);
    g.rx_pos += k;
    got += k;
  }
  return (int)n;
}

static void client_cb(const usb_host_client_event_msg_t *m, void *arg) {
  (void)arg;
  if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) g.addr = m->new_dev.address;
  if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) ESP_LOGW(TAG, "device gone");
}

static void lib_task(void *a) {
  (void)a;
  for (;;) usb_host_lib_handle_events(portMAX_DELAY, NULL);
}
static void client_task(void *a) {
  (void)a;
  for (;;) usb_host_client_handle_events(g.client, portMAX_DELAY);
}

/* Walk the configuration descriptor, print every interface/endpoint, remember the first IIO interface's bulk pair. */
static bool scan_config(const usb_config_desc_t *cfg) {
  const uint8_t *p = (const uint8_t *)cfg, *end = p + cfg->wTotalLength;
  int cur_if = -1;
  bool cur_iio = false, found = false, have_in = false, have_out = false;
  while (p + 2 <= end && p[0]) {
    if (p[1] == 4) {
      cur_if = p[2];
      /* The v2.3 Pluto's libiio interface is class 02/00/00 with three bulk IN/OUT pairs (six endpoints). */
      cur_iio = (p[5] == IIO_USB_CLASS && p[6] == IIO_USB_SUBCLASS) || p[4] >= 4;
      ESP_LOGI(TAG, "interface %d alt %d eps %d class %02x sub %02x proto %02x%s", p[2], p[3], p[4], p[5], p[6], p[7],
               cur_iio ? "  <- IIO" : "");
    } else if (p[1] == 5) {
      uint8_t addr = p[2], attr = p[3];
      uint16_t mps = (uint16_t)(p[4] | (p[5] << 8));
      ESP_LOGI(TAG, "  endpoint %02x attr %02x mps %u", addr, attr, mps);
      if (cur_iio && !found && (attr & 3) == 2) {
        if (addr & 0x80) { g.ep_in = addr; have_in = true; } else { g.ep_out = addr; have_out = true; }
        if (have_in && have_out) { g.if_num = (uint8_t)cur_if; found = true; }
      }
    }
    p += p[0];
  }
  return found;
}

void app_main(void) {
  g.done = xSemaphoreCreateBinary();
  usb_host_config_t hc = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
  ESP_ERROR_CHECK(usb_host_install(&hc));
  xTaskCreate(lib_task, "usb_lib", 4096, NULL, 10, NULL);
  usb_host_client_config_t cc = {.is_synchronous = false, .max_num_event_msg = 5, .async = {.client_event_callback = client_cb}};
  ESP_ERROR_CHECK(usb_host_client_register(&cc, &g.client));
  xTaskCreate(client_task, "usb_cl", 4096, NULL, 9, NULL);
  ESP_ERROR_CHECK(usb_host_transfer_alloc(RX_CHUNK + 64, 0, &g.xfer));
  g.rx = heap_caps_malloc(RX_CHUNK, MALLOC_CAP_8BIT);

  ESP_LOGI(TAG, "waiting for a USB device on the host port...");
  while (!g.addr) vTaskDelay(pdMS_TO_TICKS(200));
  vTaskDelay(pdMS_TO_TICKS(300));
  ESP_ERROR_CHECK(usb_host_device_open(g.client, g.addr, &g.dev));
  const usb_device_desc_t *dd;
  ESP_ERROR_CHECK(usb_host_get_device_descriptor(g.dev, &dd));
  ESP_LOGI(TAG, "device vid %04x pid %04x class %02x", dd->idVendor, dd->idProduct, dd->bDeviceClass);
  const usb_config_desc_t *cfg;
  ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(g.dev, &cfg));
  if (!scan_config(cfg)) {
    ESP_LOGE(TAG, "no IIO (class ff/eb) interface with a bulk pair found; see the list above");
    return;
  }
  ESP_LOGI(TAG, "using interface %d in %02x out %02x", g.if_num, g.ep_in, g.ep_out);
  ESP_ERROR_CHECK(usb_host_interface_claim(g.client, g.dev, g.if_num, 0));
  ESP_LOGI(TAG, "reset pipes -> %d", ctrl_out(CMD_RESET_PIPES, 0, g.if_num));
  int r = ctrl_out(CMD_OPEN_PIPE, 0, g.if_num);
  ESP_LOGI(TAG, "open pipe 0 -> %d", r);

  pluto_io_t io = {u_send, u_recv, NULL};
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
  ESP_LOGI(TAG, "done");
}
