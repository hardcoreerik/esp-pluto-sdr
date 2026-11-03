#ifndef ESP_PLUTO_TCP_H
#define ESP_PLUTO_TCP_H
/** @file esp_pluto_tcp.h
 * @brief Application-owned lwIP/POSIX IPv4 transport for the IIOD client.
 */
#include "esp_pluto_sdr.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Socket owner. Initialize with { .fd = -1 }; do not copy live instances. */
typedef struct {
  int fd;
} pluto_tcp;
/** Connect numeric IPv4 (no DNS/IPv6), returning borrowed callbacks in io.
 * @param port Nonzero TCP port; standard IIOD commonly uses 30431.
 * @param timeout_ms 1..60000; bounds connect wait and each send/recv, NOT a
 * complete command. OS scheduling may extend elapsed wall time.
 * @return 0, PLUTO_INVALID for invalid arguments/address, or PLUTO_IO for
 * socket/connect/configuration failure. Errors do not expose errno.
 * Disconnect any previous socket first; reusing a live owner leaks its fd.
 * Owns no netif, DHCP, Ethernet PHY, USB host, power controls or Wi-Fi setup.
 * No TLS/authentication is provided. Do not free/move owner while IO uses it.
 */
int pluto_tcp_connect(pluto_tcp *transport, const char *numeric_ipv4,
                      uint16_t port, unsigned timeout_ms, pluto_io *io);
/** Shut down/close fd and set it to -1; NULL or fd==-1 is a no-op.
 * Does not issue IIOD CLOSE or update a client. After disconnect do not reuse
 * old callbacks. Serialize with operations; concurrent cancellation unsupported.
 */
void pluto_tcp_disconnect(pluto_tcp *transport);
#ifdef __cplusplus
}
#endif
#endif
