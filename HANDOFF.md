# esp-pluto-sdr research handoff

Branch: chatgpt/esp-pluto-sdr-rsa-rea-20261102

Research documents and pinned RSA evidence are under docs/research/. Start with
docs/research/README.md. The branch contains research only; nothing to flash yet.

Initial target is ESP32-P4 hosting a verified Pluto-compatible radio. OrcSDR will
consume the independent component via an application-owned adapter.

Executed: source imports, RSA comparison, offline Fishball DSP and safety scripts.
Pending: physical board identity, exact firmware, cross-build, protocol fixtures,
Ethernet and USB streaming, RF comparison, Soak/recovery and optional TX review.

Next implementation step: select the legacy-compatible IIOD client approach
after a small memory/dependency/license comparison, then build an RX-only
context/control example over an existing IP connection. Validate RNDIS separately.

No merge is requested or performed. No compatibility or maximum-rate release
claim is justified yet. See EVIDENCE.md for execution limitations and failures.

User-confirmed firmware source: matsvandamme/fishball7020-fpga-devkit.
Prioritize this firmware for initial host compatibility; installed version and
factory/modern variant remain to be captured. Tezuka remains comparative research.
