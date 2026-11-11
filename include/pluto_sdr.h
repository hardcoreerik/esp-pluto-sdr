#pragma once
/* PlutoSDR (AD9361) receiver driver for ESP-IDF over USB: lifecycle, typed controls, capabilities, streaming and metrics, in the shape of
 * the esp-rtl-sdr driver (config_default / install / start / stop / read, get_state, get_capabilities, get_metrics, err_to_name) so an
 * application can treat both alike. It does not depend on esp-rtl-sdr or on any application.
 *
 * Receive only: no transmit attribute is ever written. The hardware has TX; this driver reports that in the capabilities and leaves it off.
 *
 * Threading: install() creates one worker task that owns the USB pipe (scanning, configuring, reading). The control calls below are safe from
 * any task and are applied by the worker. read() is for a single consumer task. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLUTO_SDR_VERSION_STRING "0.3.0"

typedef enum {
  PLUTO_SDR_OK = 0,
  PLUTO_SDR_ERR_INVALID_ARG = -1,
  PLUTO_SDR_ERR_NOT_CONNECTED = -2,
  PLUTO_SDR_ERR_TIMEOUT = -3,
  PLUTO_SDR_ERR_BAD_FREQ = -4,
  PLUTO_SDR_ERR_BAD_RATE = -5,
  PLUTO_SDR_ERR_BAD_STATE = -6,
  PLUTO_SDR_ERR_NO_MEM = -7,
  PLUTO_SDR_ERR_IO = -8,
  PLUTO_SDR_ERR_UNSUPPORTED = -9,
  PLUTO_SDR_ERR_BAD_GAIN = -10,
} pluto_sdr_err_t;

typedef enum {
  PLUTO_SDR_STATE_IDLE = 0,    /* installed, no Pluto on the bus */
  PLUTO_SDR_STATE_PRESENT,     /* a Pluto is attached and held open; not started */
  PLUTO_SDR_STATE_OPENING,     /* started: claiming the pipe, reading ranges, applying the configuration */
  PLUTO_SDR_STATE_STREAMING,
  PLUTO_SDR_STATE_ERROR,       /* the last session failed; the worker retries after a pause while started */
} pluto_sdr_state_t;

typedef enum {
  PLUTO_SDR_FORMAT_CU8 = 0,    /* unsigned 8-bit I/Q, offset 128: the RTL-SDR format. 12-bit samples are reduced to 8 bits. */
  PLUTO_SDR_FORMAT_CS16 = 1,   /* signed 16-bit I/Q as the device delivers it (12-bit, right aligned, full scale +-2048); no decimation */
} pluto_sdr_format_t;

typedef enum {
  PLUTO_SDR_EVENT_ATTACHED = 1,
  PLUTO_SDR_EVENT_DETACHED,
  PLUTO_SDR_EVENT_STREAM_STARTED,
  PLUTO_SDR_EVENT_STREAM_STOPPED,
  PLUTO_SDR_EVENT_RATE_CHANGED,
} pluto_sdr_event_t;

typedef struct pluto_sdr *pluto_sdr_handle_t;
typedef void (*pluto_sdr_event_cb_t)(pluto_sdr_handle_t h, pluto_sdr_event_t ev, void *ctx); /* called from the worker task; must not block */

typedef struct {
  bool install_usb_host;        /* true: this driver installs the USB host library; false: someone else already did (a client is added) */
  pluto_sdr_format_t format;
  uint32_t read_samples;        /* complex samples per USB buffer read (default 16384) */
  uint8_t buffers;              /* raw blocks between the worker and read() (default 4) */
  uint8_t max_decimation;       /* highest integer decimation for CU8 output (default 20, so about 104 kS/s is the lowest output rate); 0 = no cap */
  uint32_t link_max_sps;        /* highest converter rate the host link may be asked for; default 5,500,000 (measured on an ESP32-P4: clean to 5.5 MS/s) */
  uint8_t worker_core;          /* -1 for no affinity */
  uint8_t worker_priority;      /* default 7: the USB reader must not wait behind conversion or DSP */
  pluto_sdr_event_cb_t event_cb;
  void *event_ctx;
} pluto_sdr_config_t;

void pluto_sdr_config_default(pluto_sdr_config_t *cfg);

/* What the attached device can do, as it reports it (zeros until a session has read it). */
typedef struct {
  bool valid;
  uint64_t tune_min_hz, tune_max_hz;   /* RX LO span */
  uint32_t rate_min_sps, rate_max_sps; /* converter rate span the device allows */
  uint32_t rate_link_max_sps;          /* what the link is allowed to carry (config.link_max_sps) */
  uint32_t out_rate_min_sps;           /* lowest output rate this driver will plan (device minimum divided by the decimation cap; CS16 cannot go below rate_min_sps) */
  uint32_t bw_min_hz, bw_max_hz;       /* analog RF bandwidth span */
  int gain_min_db10, gain_max_db10;    /* manual gain span, tenths of a dB */
  int32_t ppm_min_milli, ppm_max_milli; /* reference-oscillator correction span, thousandths of a ppm (about +-200 ppm) */
  uint8_t adc_bits;                    /* 12 */
  uint8_t rx_channels;                 /* 2 on the AD9361 (this driver streams RX1) */
  bool gain_auto;                      /* the device AGC can be selected */
  bool hardware_tx;                    /* the device has transmit outputs */
  bool tx_enabled;                     /* always false: this driver is receive only */
} pluto_sdr_caps_t;

typedef struct {
  char manufacturer[32], product[48], serial[40];
  uint16_t vid, pid;
  bool high_speed;
} pluto_sdr_info_t;

typedef struct {
  bool manual;
  int gain_db10;
  int steps_db10[32];
  uint8_t step_count;
} pluto_sdr_gain_t;

/* Everything the driver last read back from the device, so a caller (or a test) can compare what it asked for with what the hardware is
 * doing. Refreshed one attribute at a time while streaming, so values can be up to about a second old. */
typedef struct {
  bool valid;                          /* a session has refreshed it */
  uint64_t center_requested_hz;        /* the last frequency asked for */
  uint64_t center_actual_hz;           /* the RX LO as the device reports it */
  int64_t center_error_hz;             /* actual - requested */
  uint32_t rate_requested_sps;         /* the output rate asked for */
  uint32_t rate_out_sps;               /* the output rate running (nominal) */
  uint32_t rate_converter_sps;         /* the AD9361 sample rate as the device reports it (device rounding visible here) */
  uint32_t decimation;
  uint32_t bandwidth_hz;               /* analog RF bandwidth as the device reports it */
  bool gain_manual;
  int gain_db10;                       /* RX1 gain, tenths of a dB */
  int rssi_db10;                       /* RX1 RSSI, tenths of a dB (the device's own relative figure) */
  int32_t ppm_milli;                   /* reference correction in thousandths of a ppm (positive = the oscillator runs fast; rtl_sdr's sign) */
  uint32_t xo_hz;                      /* reference frequency programmed into the device */
  bool dc_tracking_rf, dc_tracking_bb, quadrature_tracking;
} pluto_sdr_controls_t;

typedef struct {
  uint64_t samples_received;   /* output-rate samples that came off the USB pipe */
  uint64_t samples_delivered;  /* samples handed out by read() */
  uint32_t source_drops;       /* USB blocks discarded because every raw buffer was in use */
  uint32_t stale_blocks;       /* blocks discarded by a rate change before they were converted */
  uint32_t usb_errors;         /* sessions ended by a read error */
  uint32_t sessions;           /* sessions opened since install */
  uint32_t effective_in_sps;   /* received rate over the last window */
  uint32_t effective_out_sps;  /* delivered rate over the last window */
} pluto_sdr_metrics_t;

/* Describes one block returned by read(). */
typedef struct {
  pluto_sdr_format_t format;
  uint32_t sample_rate;        /* S/s of this block */
  uint64_t center_hz;          /* LO frequency this block was read under */
  uint32_t generation;         /* +1 on every start, stop, rate change or reopen: a change means "not the same stream as before" */
  uint32_t sequence;           /* +1 per delivered block within the install */
  int64_t time_us;             /* esp_timer time when the block was read off USB (not RF-synchronous) */
  uint32_t samples;            /* complex samples in the block */
} pluto_sdr_block_t;

int pluto_sdr_install(const pluto_sdr_config_t *cfg, pluto_sdr_handle_t *out);
int pluto_sdr_uninstall(pluto_sdr_handle_t h);

/* Streaming is off until started. start() asks the worker to claim the pipe and stream whenever a Pluto is present; stop() ends the session
 * (buffer closed, USB released) and waits for it, so a caller can hand the USB bus to something else deterministically. */
int pluto_sdr_start(pluto_sdr_handle_t h);
int pluto_sdr_stop(pluto_sdr_handle_t h, uint32_t timeout_ms);

pluto_sdr_state_t pluto_sdr_get_state(pluto_sdr_handle_t h);
bool pluto_sdr_device_present(pluto_sdr_handle_t h);
int pluto_sdr_get_capabilities(pluto_sdr_handle_t h, pluto_sdr_caps_t *out);
int pluto_sdr_get_device_info(pluto_sdr_handle_t h, pluto_sdr_info_t *out);
int pluto_sdr_get_metrics(pluto_sdr_handle_t h, pluto_sdr_metrics_t *out);

/* Controls. Values are checked against the capabilities once a session has read them (before that they are accepted and checked when applied).
 * They are applied by the worker; the result of a retune is read back with get_tune_result(). The rate is the wanted OUTPUT rate: the driver
 * chooses the converter rate and an integer decimation (CU8 only) and keeps the nominal rate when the device rounds by 2 Hz or less. */
int pluto_sdr_set_center_freq(pluto_sdr_handle_t h, uint64_t hz);
uint64_t pluto_sdr_get_center_freq(pluto_sdr_handle_t h);
int pluto_sdr_get_tune_result(pluto_sdr_handle_t h, uint64_t *hz, int *err); /* the device's answer to the last retune (0 = accepted) */
int pluto_sdr_set_sample_rate(pluto_sdr_handle_t h, uint32_t out_sps);
uint32_t pluto_sdr_get_sample_rate(pluto_sdr_handle_t h);                   /* the output rate currently running (0 when not streaming) */
int pluto_sdr_set_rf_bandwidth(pluto_sdr_handle_t h, uint32_t hz);          /* 0 = automatic (80% of the output rate) */
int pluto_sdr_set_gain_auto(pluto_sdr_handle_t h);
int pluto_sdr_set_gain_manual(pluto_sdr_handle_t h, int gain_db10);
int pluto_sdr_get_gain(pluto_sdr_handle_t h, pluto_sdr_gain_t *out);
int pluto_sdr_get_controls(pluto_sdr_handle_t h, pluto_sdr_controls_t *out);

/* Reference-oscillator correction for frequency accuracy, in thousandths of a ppm (the AD9361 takes the true reference frequency; its step is
 * 1 Hz of 40 MHz = 0.025 ppm). Use it when a known signal shows the Pluto's tuning is off by a constant fraction. */
int pluto_sdr_set_freq_correction(pluto_sdr_handle_t h, int32_t ppm_milli);

/* The device's offset trackers: RF DC, baseband DC and quadrature. They are on in the device's default; turn one off to measure the raw error. */
int pluto_sdr_set_tracking(pluto_sdr_handle_t h, bool dc_rf, bool dc_bb, bool quadrature);

/* The output rate the driver would run for `wanted` (it plans decimation and the link ceiling): the parity of esp_rtl_sdr_quantize_sample_rate.
 * Returns 0 and fills `applied`, or a negative error (BAD_RATE). Needs a session to have read the device's rate span once. */
int pluto_sdr_quantize_sample_rate(pluto_sdr_handle_t h, uint32_t wanted, uint32_t *applied);

/* Upper bound for `dst_cap` of read(): one USB block in the configured format at the lowest decimation. */
size_t pluto_sdr_max_block_bytes(pluto_sdr_handle_t h);

/* Returns the number of bytes written to dst (one converted block), PLUTO_SDR_ERR_TIMEOUT when no block arrived within timeout_ms, or
 * PLUTO_SDR_ERR_NOT_CONNECTED when not streaming. One consumer task only. */
int pluto_sdr_read(pluto_sdr_handle_t h, void *dst, size_t dst_cap, uint32_t timeout_ms, pluto_sdr_block_t *meta);

const char *pluto_sdr_err_to_name(int err);
const char *pluto_sdr_state_to_name(pluto_sdr_state_t s);
const char *pluto_sdr_get_version_string(void);

#ifdef __cplusplus
}
#endif
