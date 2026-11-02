# esp-pluto-sdr RSA/REA research

Snapshot: 2026-11-02.

Erik confirmed on 2026-11-02 that his device runs firmware from
`matsvandamme/fishball7020-fpga-devkit`. This is the primary compatibility
target; Tezuka is a comparison target. Installed release/commit, factory versus
modern target, FPGA image and USB mode remain unverified. This is a research branch, not a working driver release.

- [Firmware review](FIRMWARE_REVIEW.md): Fishball family, Tezuka, Fishball DATV and ADI baseline.
- [Host architecture](HOST_ARCHITECTURE.md): IIOD compatibility, transport, IQ representation and lifecycle.
- [Validation plan](VALIDATION.md): offline and hardware acceptance gates.
- [Evidence and limitations](EVIDENCE.md): actual commands, failures, tools and outstanding work.
- [Pinned sources](sources.json), [RSA metrics](metrics/metrics.csv), [generated comparison](metrics/report.md).

The standalone component must never depend on OrcSDR. OrcSDR will own its adapter.
Source findings establish design constraints; they do not establish compatibility with Erik's physical board.

Latest executed analysis: [RSA/REA pass 2](pass2/README.md).
