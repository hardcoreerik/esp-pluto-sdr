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
