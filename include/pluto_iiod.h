#pragma once
/* Minimal iiod (libiio network daemon, protocol 0.26 text mode) client for PlutoSDR-compatible radios.
 * Transport-agnostic: the caller supplies send/recv, so the same code runs on lwIP, POSIX sockets or a test double.
 * Receive-oriented: attribute read/write plus one RX buffer stream. No dependency on OrcSDR or on libiio. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  /* Send all n bytes; return n or a negative errno. */
  int (*send)(void *ctx, const void *buf, size_t n);
  /* Receive exactly n bytes (block until done); return n or a negative errno (0 or less = closed/timeout). */
  int (*recv)(void *ctx, void *buf, size_t n);
  void *ctx;
} pluto_io_t;

#define PLUTO_ID_MAX 32

typedef struct {
  pluto_io_t io;
  char phy[PLUTO_ID_MAX];  /* iio device id of ad9361-phy, e.g. "iio:device0" */
  char rx[PLUTO_ID_MAX];   /* iio device id of cf-ad9361-lpc (RX DMA), e.g. "iio:device3" */
  bool buffer_open;
  uint32_t mask_chans;     /* channels enabled in the open buffer */
} pluto_iiod_t;

enum { PLUTO_OK = 0, PLUTO_ERR_IO = -5, PLUTO_ERR_PROTO = -71, PLUTO_ERR_NODEV = -19, PLUTO_ERR_RANGE = -34 };

/* Handshake: VERSION, then PRINT (streamed, not stored) to find the phy and RX device ids. */
int pluto_iiod_open(pluto_iiod_t *c, const pluto_io_t *io);

/* Device attribute (channel == NULL) or channel attribute. `output` selects the OUTPUT direction (e.g. the RX LO is
 * altvoltage0 OUTPUT). Return value on write: bytes accepted (>=0) or a negative iiod error. */
int pluto_iiod_write_attr(pluto_iiod_t *c, const char *dev, const char *channel, bool output, const char *attr,
                          const char *value);
/* Returns length of the value (NUL-terminated into out) or a negative error. */
int pluto_iiod_read_attr(pluto_iiod_t *c, const char *dev, const char *channel, bool output, const char *attr,
                         char *out, size_t cap);

/* RX buffer: `chan_mask` bit i = channel voltage<i> (0..3). 0x3 = RX1 I and Q (4 bytes per sample). */
int pluto_iiod_buffer_open(pluto_iiod_t *c, uint32_t samples, uint32_t chan_mask);
/* Reads up to `bytes` of the buffer into dst; returns bytes read or a negative error. */
int pluto_iiod_buffer_read(pluto_iiod_t *c, void *dst, size_t bytes);
int pluto_iiod_buffer_close(pluto_iiod_t *c);

#ifdef __cplusplus
}
#endif
