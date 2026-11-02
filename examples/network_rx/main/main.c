#include "esp_log.h"
#include "esp_pluto_sdr.h"
#include "esp_pluto_tcp.h"
/* Bring up the board's Ethernet or USB RNDIS netif before running this task.
 * The component deliberately does not create or own application networking. */
void app_main(void) {
  ESP_LOGI("pluto", "Bring up an IP netif, then call pluto_receive_example()");
}
/* IDs/mask must come from the actual IIO context and confirmed input scan
 * layout. Raw scan bytes remain caller-owned; no assumptions about signedness
 * or stride. */
int pluto_receive_example(const char *numeric_ip, const char *rx_device,
                          uint32_t mask) {
  pluto_tcp transport;
  pluto_io io;
  int r = pluto_tcp_connect(&transport, numeric_ip, 30431, 2000, &io);
  if (r)
    return r;
  pluto_client radio;
  pluto_init(&radio, io);
  static char xml[32768];
  size_t count;
  r = pluto_context(&radio, xml, sizeof(xml), &count);
  if (!r)
    ESP_LOGI("pluto", "Context received: %u bytes", (unsigned)count);
  if (!r)
    r = pluto_rx_open(&radio, rx_device, 4096, mask);
  static unsigned char samples[16384];
  for (int block = 0; !r && block < 10; ++block) {
    r = pluto_rx_read(&radio, samples, sizeof(samples), &count);
    if (!r)
      ESP_LOGI("pluto", "Received %u raw scan bytes", (unsigned)count);
  }
  if (radio.streaming) {
    int close_result = pluto_rx_close(&radio);
    if (!r)
      r = close_result;
  }
  pluto_tcp_disconnect(&transport);
  return r;
}
