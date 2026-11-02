# Proposed host architecture

Status: research proposal, not an implemented API. Sources: pinned libiio
iiod-client.c, network.c, examples/ad9361-iiostream.c and FIRMWARE_REVIEW.md.

## Dependency direction

esp-pluto-sdr is an ESP-IDF component. OrcSDR owns its source adapter and any
conversion to its DSP contract. The driver must compile without OrcSDR, UI,
dashboards, demodulators or an RTL-SDR driver. No change to those repos is included.

Separate transport/socket IO, IIOD framing/context parsing, AD936x capability
mapping, RX lifecycle, sample conversion and health accounting. Keep USB netif
ownership injectable so applications can use an existing Ethernet/network stack.

## Protocol baseline

libiio/iiod-client.c supports legacy textual commands and a negotiated BINARY
path; it handles PRINT/ZPRINT, attribute READ/WRITE, OPEN and READBUF.
Match the running server rather than assuming libiio 1.0 on the radio.
Prototype both an upstream libiio network adaptation and a bounded legacy client;
compare flash/RAM, dependency and license costs before selecting implementation.
Zephyr's server/backend integration is useful architecture reference, not an
already-complete ESP-IDF host client.

A correct client needs partial send/receive loops, explicit deadlines,
signed error/result parsing, bounded XML and payload lengths, channel-mask
validation and orderly CLOSE/cancel/disconnect handling. TCP receive boundaries
are not IIO block boundaries. Do not implement READBUF as a single recv().
Fuzz malformed lengths, negative returns, truncated XML, block fragmentation,
coalescing, mask changes, reconnect and cancellation while a read is blocked.

## Radio capability mapping

The ADI example uses ad9361-phy for configuration, cf-ad9361-lpc for RX and
cf-ad9361-dds-core-lpc for TX. Treat names as discovery hints and validate
attributes and direction. Frequency uses appropriate LO output channels;
gain/sample-rate/RF bandwidth belong to distinct attributes. Read back every
setting and expose requested versus applied values. Retuning or rate changes
must have a defined stream restart/discontinuity boundary.

Discover enabled input channels, scan indices, storage/significant bits,
shift, signedness, byte order, repeat count and stride. A 12-bit ADC is not
a promise of a fixed 12-bit wire format. Interleaved dual RX is distinct
from one I/Q pair. Reject unsupported formats explicitly.

## Transport and memory budget

For two signed 16-bit scalar values per complex sample:

| Rate per RX channel | One RX payload | Two RX payload |
|---|---:|---:|
| 0.5 MS/s | 2.0 MB/s | 4.0 MB/s |
| 1.0 MS/s | 4.0 MB/s | 8.0 MB/s |
| 2.4 MS/s | 9.6 MB/s | 19.2 MB/s |
| 3.2 MS/s | 12.8 MB/s | 25.6 MB/s |

These are arithmetic requirements, not measured sustainable rates. Formula:
rate * channels * 2 scalar components * 2 bytes; decimal MB/s. Duplex adds
both directions and protocol overhead. A 4096-complex-sample single-channel
block is 16 KiB; four such blocks consume 64 KiB before transport/DSP allocations.
Choose bounded queues and explicit release/ownership rules; expose backpressure
and discontinuities rather than silently dropping oldest data.

Espressif iot_usbh_rndis 0.5.2 is a candidate. The registry documents P4 support,
and its changelog includes reconnect fixes. Official P4 host documentation
supports composite clients, but actual RNDIS descriptor matching and endpoint
allocation must be tested on the chosen image. RNDIS does not cover ECM/NCM.
Do not assume DHCP availability or a universal fixed IP; allow explicit address
configuration, then optional discovery. Avoid hardcoded interface number zero.

## Lifecycle and observability

Proposed states: disconnected, connecting, ready, streaming, stopping, fault.
One owner serializes control operations; cancellation must unblock streaming.
Report transport bytes/blocks, valid complex samples, queue high-water mark,
timeouts, disconnects, rejected formats, local discarded samples and epochs.
Do not claim RF/DMA loss is zero merely because TCP delivered all bytes: standard
streams may lack a device sequence counter. Distinguish measurable transport
continuity from unobservable upstream loss.

RX-only first. TX later needs separate enable/state, intentional start, explicit
stop, firmware capability checks and device-side watchdog testing. An ESP host
disconnect cannot itself guarantee RF silence on an arbitrary firmware.

Primary references:
- https://components.espressif.com/components/espressif/iot_usbh_rndis/versions/0.5.2/readme
- https://components.espressif.com/components/espressif/iot_usbh_rndis/versions/0.5.2/changelog
- https://docs.espressif.com/projects/esp-usb/en/latest/esp32p4/usb_host.html
