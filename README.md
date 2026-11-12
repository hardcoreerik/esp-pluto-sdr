# esp-pluto-sdr
Independent ESP-IDF host component for PlutoSDR-compatible IIO radios. ESP32-P4 first, with radio control and IQ reception; OrcSDR is the first integration target.

## Status (0.1.0, 2026-11-09)

- `include/pluto_iiod.h`, `src/pluto_iiod.c`: iiod text-protocol (0.26) client. Transport-agnostic (you supply send/recv), finds
  `ad9361-phy` and `cf-ad9361-lpc`, reads/writes attributes, opens an RX buffer and reads it. Protocol details learned from a
  real board: `READBUF` answers with a length line, then a channel-mask line (`00000003`), then the data.
- `include/pluto_iq.h`, `src/pluto_iq.c`: int16 12-bit IQ (full scale +-2048) to u8 offset-128 IQ, with integer decimation.
- `tests/run.sh [radio-ip]`: pure tests, plus a **receive-only** live smoke test (sets RX rate/LO, reads 40 buffers).
  Verified 2026-11-09 on a FISH Ball PlutoSDR (fw v2.3) over RJ45: 11.45 MB/s at 2.88 MS/s, rate/LO readback correct.

## Link budget (read this before choosing a host)

The AD9361 cannot run below about 2.083 MS/s, so the wire carries at least 8.3 MB/s (4 bytes per complex sample over iiod).
A wired host (ESP32-P4 board with Ethernet) can take that. An ESP32-P4 behind Wi-Fi through an ESP-Hosted C6 (Tab5) tops out
near 2 MB/s, so it cannot receive the Pluto in real time; decimation would have to happen on the Pluto.

## USB host path (verified 2026-11-09)

`examples/usb_probe` runs on an ESP32-P4 USB host, finds the Pluto (vid 0456 pid b673, high speed), claims the libiio
interface, opens pipe 0 with vendor control requests (RESET_PIPES=0, OPEN_PIPE=1; bmRequestType 0x41, wValue = pipe,
wIndex = interface) and runs the same iiod client over the bulk endpoints. Result on a Waveshare ESP32-P4-WIFI6-POE-ETH
(chip rev v3.2) with the Pluto (fw v2.3) on its USB port: **11.51 MB/s for 5 s at 2.88 MS/s (needs 11.52), no stall**.

Findings:
- v2.3 exposes two CDC-ACM serial pairs (interfaces 0-3) and the libiio interface as **interface 4, class 02/00/00, six bulk
  endpoints = three pipes** (85/03, 86/04, 87/05). It is not class FF/EB, so match on the endpoint count.
- Control transfers on the P4 host must use `usb_host_transfer_submit_control()`.
- On this chip (rev v3.2) the default v3 image only boots with `CONFIG_ESPTOOLPY_FLASHFREQ_40M`; at 80 MHz the ROM rejects the
  bootloader ("mismatch chip ID"). The "older than v3" image setting the other P4 boards use is refused by esptool here.

## USB throughput ceiling on the ESP32-P4 (2026-11-10)

`examples/usb_ladder` raises the AD9361 rate and pulls 16-bit IQ off the Pluto's libiio USB pipe with nothing else running (one 16 KiB transfer in
flight at a time, no conversion). Waveshare ESP32-P4-WIFI6-POE-ETH (rev v3.2), Pluto fw v2.3, USB 2.0 high speed:

| Asked | Delivered | Of nominal |
|---|---|---|
| 2.4 to 5.5 MS/s | equal to nominal (9.6 to 22.0 MB/s) | 100% |
| 6.0 MS/s | 5.82 MS/s (23.3 MB/s) | 97% |
| 6.5 / 7.0 / 8.0 MS/s | 5.82 to 5.84 MS/s (23.3 MB/s) | 89% / 83% / 73% |

The delivered rate flattens at about 23.3 MB/s, the same ceiling seen from a PC, so the limit looks like the Pluto's USB gadget and not this
host or this transport. Loss above the ceiling is silent on the wire (the reads just return fewer samples per second than nominal); compare the
delivered rate with the nominal one to detect it. About 5.5 MS/s of 16-bit IQ is the highest clean rate measured; 7 MS/s is not reachable over
this USB link. Not tried: two transfers in flight, larger transfers, another Pluto firmware.

## Tested on the M5Stack Tab5 (ESP32-P4 rev v1.3), 2026-11-11

`examples/pluto_sdr_tab5` is a standalone test that uses only `pluto_sdr.h` (plus one board file that switches the Tab5's USB-A 5 V rail through its
I/O expander). Results with a Pluto on the Tab5's USB-A port, USB 2.0 high speed:

- 43 checks, 0 failures, in three runs, one of them straight after an abrupt reset of the Tab5 in the middle of streaming.
- 8-bit output at 0.96 and 2.4 MS/s delivered at 100.00% of nominal, no dropped blocks, no USB errors.
- Tuning at five bands, manual and automatic gain, RF bandwidth, reference correction (+-5 ppm) and the offset trackers all read back correctly on the
  device, and out-of-range requests are refused.
- Native 16-bit rate ladder (delivered / nominal): 2.4, 3, 4 and 5 MS/s 100%; 5.5 MS/s 97.9%; 6 MS/s 89.7%; 7 MS/s 77.1%. Delivery flattens at about
  21.5 MB/s, so about 5 MS/s is the highest clean rate here (the ESP32-P4 board measured earlier flattened at 23.3 MB/s).

Finding that changed the driver: the transport closed without sending libiio's `CLOSE_PIPE` request. After a clean session end the Pluto could keep the
pipe open for a host that had gone, and the next session's first request went unanswered until iiod was restarted. The transport now sends
`CLOSE_PIPE` before releasing the interface, as libiio does. With it, back-to-back sessions and a session after an abrupt host reset all connect.

Board note (Tab5): a reset of the ESP32-P4 does not switch off the I/O-expander USB-A rail, so a Pluto stays powered across a reflash.
