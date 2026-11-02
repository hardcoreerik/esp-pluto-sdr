#ifndef ESP_PLUTO_SDR_H
#define ESP_PLUTO_SDR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Callbacks return transferred bytes (>0), or a negative error. They must have
 * bounded deadlines. One owner/task per client; reconnect after any fault. */
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
typedef struct {
  pluto_io io;
  bool streaming, faulted;
  char device[64];
  uint32_t mask;
  uint64_t bytes_received;
} pluto_client;
void pluto_init(pluto_client *, pluto_io);
int pluto_context(pluto_client *, char *, size_t, size_t *);
/* Individual device/direction/channel/attribute tokens are validated.
 * The caller selects the discovered channel and attribute explicitly. */
int pluto_attr_write(pluto_client *, const char *device, const char *direction,
                     const char *channel, const char *attribute,
                     const char *value);
/* Explicit scan mask (up to 32 scalar channels). Caller must first inspect
 * context scan formats. Output is raw interleaved scan bytes, not normalized
 * IQ. */
int pluto_rx_open(pluto_client *, const char *, size_t, uint32_t);
int pluto_rx_read(pluto_client *, void *, size_t, size_t *);
int pluto_rx_close(pluto_client *);
#ifdef __cplusplus
}
#endif
#endif
