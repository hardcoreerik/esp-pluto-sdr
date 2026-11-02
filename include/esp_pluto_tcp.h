#ifndef ESP_PLUTO_TCP_H
#define ESP_PLUTO_TCP_H
#include "esp_pluto_sdr.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  int fd;
} pluto_tcp;
/* Numeric IPv4 only; timeout bounds connect and each socket IO call.
 * Initialize a fresh instance; disconnect before reusing it. Owns no netif. */
int pluto_tcp_connect(pluto_tcp *, const char *, uint16_t, unsigned,
                      pluto_io *);
void pluto_tcp_disconnect(pluto_tcp *);
#ifdef __cplusplus
}
#endif
#endif
