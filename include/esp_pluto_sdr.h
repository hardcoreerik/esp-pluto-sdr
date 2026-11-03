#ifndef ESP_PLUTO_SDR_H
#define ESP_PLUTO_SDR_H
/** @file esp_pluto_sdr.h
 * @brief Experimental synchronous legacy-IIOD client.
 * @details See docs/API.md for lifecycle, ownership, raw scan layout and recovery.
 * No internal locks, networking setup, XML parser, IQ conversion or TX API.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Callbacks return transferred bytes (>0), or a negative error. They must have
 * bounded deadlines. One owner/task per client; reconnect after any fault. */
/** Borrowed transport. All callbacks and user state must outlive client operations.
 * Return 1..requested bytes; partial transfers are retried. Zero/negative or
 * oversized returns become PLUTO_IO and fault the client. Callback errors are
 * not preserved. Bound blocking inside callbacks; no driver-wide deadline.
 */
typedef struct {
  void *user;
  int (*read)(void *, void *, size_t);
  int (*write)(void *, const void *, size_t);
} pluto_io;
enum {
  PLUTO_INVALID = -10001,
  PLUTO_PROTOCOL = -10002,
  PLUTO_IO = -10003,
  PLUTO_STATE = -10004
};
/** Caller-owned client storage; initialize before use. Treat fields as read-only.
 * bytes_received counts complete RX payload chunks, excluding framing/context.
 * This development struct layout is not a stable binary ABI.
 */
typedef struct {
  pluto_io io;
  bool streaming, faulted;
  char device[64];
  uint32_t mask;
  uint64_t bytes_received;
} pluto_client;
/** Reset client state/counters and borrow io; allocates nothing.
 * Does not connect, validate callbacks, close an old stream or disconnect.
 * A NULL client is a no-op. Never reset a live session in place.
 */
void pluto_init(pluto_client *client, pluto_io io);
/** Fetch PRINT context XML while not streaming.
 * @param xml Caller buffer; capacity must include one NUL terminator.
 * @param length Required; reset to zero on entry, set to XML bytes on success.
 * @return 0, remote negative status, or PLUTO_* error. Oversize context faults
 * the connection; XML is neither parsed nor validated. On error ignore xml.
 */
int pluto_context(pluto_client *client, char *xml, size_t capacity, size_t *length);
/** Write a channel attribute while not streaming.
 * direction must be exactly INPUT or OUTPUT. Device/channel/attribute are
 * nonempty tokens of at most 63 ASCII letters/digits or _ : - .
 * value is NUL-terminated, at most 128 characters from digits, + - . lowercase
 * letters and underscore. Wire payload excludes its NUL; no newline is added.
 * @return 0 only if remote accepted exactly strlen(value) bytes; otherwise a
 * remote negative status or PLUTO_* error. No range/unit/readback validation.
 * OUTPUT describes IIO channel direction, including RX LO configuration.
 */
int pluto_attr_write(pluto_client *, const char *device, const char *direction,
                     const char *channel, const char *attribute,
                     const char *value);
/** Open one non-cyclic buffer on a caller-verified input device.
 * samples is the requested scan count (1..INT_MAX), not bytes. mask must be
 * nonzero; this version supports one 32-bit mask word. No discovery or device
 * direction validation occurs. An already-open stream returns PLUTO_STATE.
 * @return 0 on success; negative remote status leaves client idle unless IO or
 * framing failed. Device ID is copied; all argument strings remain caller-owned.
 */
int pluto_rx_open(pluto_client *client, const char *device, size_t samples,
                  uint32_t mask);
/** Read up to capacity raw scan bytes (1..INT_MAX) into caller storage.
 * Requires an open stream. length is required and reset to zero on entry.
 * Handles multiple chunks; the first mask must exactly match eight lowercase
 * hex digits. A server zero-length chunk ends the read successfully, possibly
 * with zero/partial output. Caller must choose scan-aligned capacity and handle
 * partial scans. No signedness, byte-order, stride or scaling conversion occurs.
 * On failure discard this call's data, even if length is nonzero: an incomplete
 * last chunk may have modified additional bytes. Remote read errors fault the
 * connection. bytes_received advances only for fully received chunks.
 */
int pluto_rx_read(pluto_client *client, void *buffer, size_t capacity,
                  size_t *length);
/** Close the remote buffer; keep the transport connected for another stream.
 * Requires streaming state. Returns 0 on success; any close error faults the
 * client. Not a socket close or asynchronous cancellation operation.
 */
int pluto_rx_close(pluto_client *client);
#ifdef __cplusplus
}
#endif
#endif
