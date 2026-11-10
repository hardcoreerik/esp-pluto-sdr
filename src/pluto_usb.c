#include "pluto_usb.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "pluto_usb";

/* libiio usb backend vendor requests (interface recipient, host to device): wValue = pipe index, wIndex = interface. */
enum { CMD_RESET_PIPES = 0, CMD_OPEN_PIPE = 1 };
enum { XFER_BYTES = 16384, SEND_CHUNK = 4096, MAX_ZERO_LENGTH = 100, MAX_NO_PROGRESS = 3 };

static struct {
  bool inited;
  usb_host_client_handle_t client;
  usb_device_handle_t dev;
  atomic_bool gone;     /* set by the client event task or a failed transfer, read by the user of the transport */
  atomic_bool claimed;
  atomic_bool broken;   /* a transfer never finished: the transport refuses I/O until the device is closed and reopened */
  uint8_t addr, if_num, ep_in, ep_out;
  SemaphoreHandle_t done;
  usb_transfer_t *xfer;
  size_t rx_len, rx_pos;
  unsigned zero_len_run;
} g;

static void xfer_cb(usb_transfer_t *t) { xSemaphoreGive((SemaphoreHandle_t)t->context); }

/* One transfer at a time, and the transfer object is never reused while the host may still own it. If the completion does not
 * arrive in time the endpoint is flushed so the host hands the transfer back (its callback then reports CANCELED); if even that
 * does not happen the transfer object is abandoned (leaked on purpose) and the transport is marked broken. */
static int run_transfer(uint8_t ep, size_t bytes, uint32_t timeout_ms) {
  if (atomic_load(&g.gone) || atomic_load(&g.broken) || !g.dev) return -19; /* ENODEV */
  while (xSemaphoreTake(g.done, 0) == pdTRUE) { /* a stale give from an earlier transfer must not satisfy this one */
  }
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
  if (xSemaphoreTake(g.done, pdMS_TO_TICKS(timeout_ms + 500)) != pdTRUE) {
    ESP_LOGW(TAG, "transfer on ep %02x did not complete in time; cancelling", ep);
    if (ep != 0) {
      (void)usb_host_endpoint_halt(g.dev, ep);
      (void)usb_host_endpoint_flush(g.dev, ep);
    }
    if (xSemaphoreTake(g.done, pdMS_TO_TICKS(1500)) != pdTRUE) {
      /* The host still owns the buffer. Never touch it again; a fresh transfer object is made when the device is reopened. */
      atomic_store(&g.broken, true);
      g.xfer = NULL;
      ESP_LOGE(TAG, "transfer on ep %02x never came back; transport marked broken", ep);
      return -110;
    }
    if (ep != 0) (void)usb_host_endpoint_clear(g.dev, ep);
    return -110;
  }
  if (g.xfer->status == USB_TRANSFER_STATUS_NO_DEVICE) atomic_store(&g.gone, true);
  return g.xfer->status == USB_TRANSFER_STATUS_COMPLETED ? g.xfer->actual_num_bytes : -(int)(100 + g.xfer->status);
}

static int ctrl_out(uint8_t req, uint16_t value, uint16_t index) {
  if (!g.xfer) return -19;
  usb_setup_packet_t *s = (usb_setup_packet_t *)g.xfer->data_buffer;
  s->bmRequestType = 0x41; /* vendor, interface, host-to-device */
  s->bRequest = req;
  s->wValue = value;
  s->wIndex = index;
  s->wLength = 0;
  return run_transfer(0, sizeof *s, 2000);
}

/* Advances by what the host actually sent: a completed but short transfer must not drop command bytes. */
static int u_send(void *ctx, const void *b, size_t n) {
  (void)ctx;
  const uint8_t *p = b;
  size_t left = n;
  int stalled = 0;
  while (left) {
    if (!g.xfer) return -19;
    size_t k = left < SEND_CHUNK ? left : SEND_CHUNK;
    memcpy(g.xfer->data_buffer, p, k);
    int r = run_transfer(g.ep_out, k, 3000);
    if (r < 0) return r;
    if (r == 0) {
      if (++stalled >= MAX_NO_PROGRESS) return -5;
      continue;
    }
    stalled = 0;
    if ((size_t)r > k) return -5;
    p += r;
    left -= (size_t)r;
  }
  return (int)n;
}

/* Bytes are consumed straight out of the transfer buffer: no second copy of the IQ stream. Zero-length packets are legal on a bulk
 * pipe but a run of them with no data is a stalled device, so the loop gives up after MAX_ZERO_LENGTH of them. */
static int u_recv(void *ctx, void *b, size_t n) {
  (void)ctx;
  uint8_t *p = b;
  size_t got = 0;
  while (got < n) {
    if (g.rx_pos == g.rx_len) {
      int r = run_transfer(g.ep_in, XFER_BYTES, 5000);
      if (r < 0) return r;
      if (r == 0) {
        if (++g.zero_len_run >= MAX_ZERO_LENGTH) {
          g.zero_len_run = 0;
          return -110;
        }
        continue;
      }
      g.zero_len_run = 0;
      g.rx_len = (size_t)r;
      g.rx_pos = 0;
    }
    size_t k = g.rx_len - g.rx_pos;
    if (k > n - got) k = n - got;
    memcpy(p + got, g.xfer->data_buffer + g.rx_pos, k);
    g.rx_pos += k;
    got += k;
  }
  return (int)n;
}

static void client_cb(const usb_host_client_event_msg_t *m, void *arg) {
  (void)arg;
  if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE && g.dev && m->dev_gone.dev_hdl == g.dev) {
    ESP_LOGW(TAG, "device gone");
    atomic_store(&g.gone, true);
  }
}

static void lib_task(void *a) {
  (void)a;
  for (;;) usb_host_lib_handle_events(portMAX_DELAY, NULL);
}

static void client_task(void *a) {
  (void)a;
  for (;;) usb_host_client_handle_events(g.client, portMAX_DELAY);
}

int pluto_usb_init(bool install_host) {
  if (g.inited) return 0;
  g.done = xSemaphoreCreateBinary();
  if (!g.done) return -12;
  if (install_host) {
    usb_host_config_t hc = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
    esp_err_t e = usb_host_install(&hc);
    if (e != ESP_OK) return -5;
    xTaskCreate(lib_task, "pluto_lib", 4096, NULL, 10, NULL);
  }
  usb_host_client_config_t cc = {.is_synchronous = false, .max_num_event_msg = 5, .async = {.client_event_callback = client_cb}};
  if (usb_host_client_register(&cc, &g.client) != ESP_OK) return -5;
  xTaskCreate(client_task, "pluto_cl", 4096, NULL, 9, NULL);
  if (usb_host_transfer_alloc(XFER_BYTES + 64, 0, &g.xfer) != ESP_OK) return -12;
  g.inited = true;
  return 0;
}

bool pluto_usb_present(void) { return g.dev != NULL && !atomic_load(&g.gone); }
bool pluto_usb_gone(void) { return atomic_load(&g.gone) || atomic_load(&g.broken); }

bool pluto_usb_poll(void) {
  if (!g.inited) return false;
  if (g.dev && !pluto_usb_gone()) return true;
  if (g.dev) pluto_usb_close(); /* unplugged or broken: forget it so a replug is found */
  uint8_t list[16];
  int n = 0;
  if (usb_host_device_addr_list_fill(sizeof list, list, &n) != ESP_OK) return false;
  for (int i = 0; i < n; ++i) {
    usb_device_handle_t d = NULL;
    if (usb_host_device_open(g.client, list[i], &d) != ESP_OK) continue;
    const usb_device_desc_t *dd = NULL;
    if (usb_host_get_device_descriptor(d, &dd) == ESP_OK && dd->idVendor == PLUTO_USB_VID && dd->idProduct == PLUTO_USB_PID) {
      g.dev = d;
      g.addr = list[i];
      atomic_store(&g.gone, false);
      atomic_store(&g.broken, false);
      ESP_LOGI(TAG, "PlutoSDR at USB address %u", g.addr);
      return true;
    }
    usb_host_device_close(g.client, d);
  }
  return false;
}

/* Walks the configuration descriptor with every length checked against the buffer. The v2.3 Pluto's libiio interface is class 02/00/00
 * with three bulk IN/OUT pairs (six endpoints); the first interface with at least four endpoints and a bulk IN and OUT is taken, and the
 * IN/OUT search restarts for every interface so endpoints of different interfaces are never paired. (libiio itself identifies the
 * interface by its "IIO" string; that needs a string-descriptor request and is not done here.) */
static bool find_iio_interface(const usb_config_desc_t *cfg) {
  const uint8_t *p = (const uint8_t *)cfg, *end = p + cfg->wTotalLength;
  int cur_if = -1;
  bool cur_iio = false, have_in = false, have_out = false;
  uint8_t in = 0, out = 0;
  while (p + 2 <= end) {
    const uint8_t len = p[0], type = p[1];
    if (len < 2 || p + len > end) return false; /* malformed: stop rather than read past the descriptor */
    if (type == 4 && len >= 9) {
      if (cur_iio && have_in && have_out) break; /* the previous interface was a match and is complete */
      cur_if = p[2];
      cur_iio = (p[5] == 0xFF && p[6] == 0xEB) || p[4] >= 4;
      have_in = have_out = false;
    } else if (type == 5 && len >= 7 && cur_iio && (p[3] & 3) == 2) {
      if (p[2] & 0x80) {
        if (!have_in) { in = p[2]; have_in = true; }
      } else if (!have_out) {
        out = p[2];
        have_out = true;
      }
    }
    p += len;
  }
  if (cur_iio && have_in && have_out && cur_if >= 0) {
    g.if_num = (uint8_t)cur_if;
    g.ep_in = in;
    g.ep_out = out;
    return true;
  }
  return false;
}

int pluto_usb_open(pluto_io_t *io) {
  if (!g.dev || pluto_usb_gone() || !io) return -19;
  if (!g.xfer && usb_host_transfer_alloc(XFER_BYTES + 64, 0, &g.xfer) != ESP_OK) return -12; /* the old one was abandoned */
  const usb_config_desc_t *cfg = NULL;
  if (usb_host_get_active_config_descriptor(g.dev, &cfg) != ESP_OK || !find_iio_interface(cfg)) {
    ESP_LOGE(TAG, "no libiio interface in the configuration");
    return -22;
  }
  if (usb_host_interface_claim(g.client, g.dev, g.if_num, 0) != ESP_OK) return -16; /* EBUSY */
  atomic_store(&g.claimed, true);
  g.rx_len = g.rx_pos = 0;
  g.zero_len_run = 0;
  int r = ctrl_out(CMD_RESET_PIPES, 0, g.if_num);
  if (r < 0) {
    pluto_usb_close();
    return r;
  }
  r = ctrl_out(CMD_OPEN_PIPE, 0, g.if_num);
  if (r < 0) {
    pluto_usb_close();
    return r;
  }
  io->send = u_send;
  io->recv = u_recv;
  io->ctx = NULL;
  ESP_LOGI(TAG, "libiio pipe open: interface %u in %02x out %02x", g.if_num, g.ep_in, g.ep_out);
  return 0;
}

void pluto_usb_close(void) {
  if (g.dev) {
    if (atomic_load(&g.claimed)) (void)usb_host_interface_release(g.client, g.dev, g.if_num);
    usb_host_device_close(g.client, g.dev);
  }
  atomic_store(&g.claimed, false);
  g.dev = NULL;
  atomic_store(&g.gone, false);
  atomic_store(&g.broken, false);
  g.rx_len = g.rx_pos = 0;
}
