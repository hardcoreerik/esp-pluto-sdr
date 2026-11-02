# Validation gates

All hardware gates are pending. Targets and thresholds below are proposed
acceptance criteria, not published performance claims.

| Gate | Procedure | Required evidence |
|---|---|---|
| Identity | Record model/context XML, firmware version, board photos/markings, channel list and USB descriptors | Hashes, exact image/FPGA revision, serial redacted in public results |
| Desktop reference | Same radio/settings with known-good libiio client | Raw IQ hash, applied settings, scan layout, known-tone FFT |
| Offline protocol | Replay legacy and negotiated sessions; fragment every boundary; inject invalid lengths/errors | Deterministic transcripts, test output, bounds/cancellation results |
| Offline samples | Signed extrema, shift, endian, stride, dual-channel fixtures, tone orientation and scaling | Fixture hashes and exact expected results |
| Ethernet P4 | Discover, tune/readback, stream one RX without PC assistance | IDF/board build hash, RAM/CPU, timeouts, throughput, spectrum comparison |
| USB P4 | Enumerate actual composite descriptor; configure link; repeat Ethernet tests | Descriptor dump, USB component lock, endpoint/interface allocation, power conditions |
| Soak | Begin at 0.5 then 1 MS/s single RX; increase only after passing; proposed 30-minute runs | Bytes/samples, queue depth, heap minimum, temperature, disconnect/error counters |
| Recovery | Unplug/replug, reboot radio, stop during blocked read, retune and change rate | Bounded stop/reconnect timing; clean resource ownership; explicit stream epochs |
| Firmware matrix | Factory candidate, Tezuka Fishball7020, independent devkit factory/modern | Separate per-image records; unsupported combinations explicit |
| Dual RX | Verify channel order, independently injected tones, decimation alias rejection and alignment | Captures for both paths and FPGA provenance |
| OrcSDR | Thin adapter with identical test input and known-signal captures | Scaling/sample-rate correctness, dashboard regressions, RTL-SDR preservation |
| TX later | Bench termination/attenuation, watchdog and disconnect measurements | Measured RF stop behavior per firmware; no assumption from host close alone |

Do not run board-connected upstream self-tests as if they were passive reads.
The Fishball self-test uses debug attributes and internal TX/BIST paths even in
its non-external-loopback mode. Its offline test_dsp.py is separate.

First milestone: the P4 alone controls the identified clone and receives
continuous IQ whose layout and RF content agree with the desktop reference.
No sample-rate maximum is promised before this evidence exists.
