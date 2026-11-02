# Executed RSA/REA pass 2 — 2026-11-02

This pass supersedes the earlier tooling-status statements in ../EVIDENCE.md.
The source commits remain those in ../sources.json. Fishball devkit remains
the user-confirmed firmware project; the device's installed image is not captured.

## Completed

- Forced fresh RSA analysis of all five targets, including the ADI baseline.
- Generated the five-target comparison and individual reports in this folder.
- Installed REA 6.8.0 and confirmed RSA doctor recognizes it.
- Copied each pinned Tezuka/Fishball_DATV Fishball7020 FSBL ELF into its RSA
  target build directory and reran `rsa.py analyze <slug> --force` without skip-rea.
- REA inspect-artifact returned exit 0 for both files. Raw artifact evidence is retained.
- RSA parsed ELF metadata and calculated hashes, sizes and section metrics.

Both bootloader files are exactly 473444 bytes with identical SHA-256:
`11fe9297125799e80ad0dac35b14f2f31beb8a2baefb59c9a46fba271e783543`.
Evidence: tezuka-fw-binaries.json and fishball-datv-binaries.json.
This establishes identical FSBL bytes in these snapshots, not identical complete
firmware, kernel, rootfs, IIOD version, FPGA bitstream or behavior.

## Commands

```sh
python rsa.py doctor
python rsa.py compare tezuka-fw fishball7020-fpga-devkit fishball-datv libiio plutosdr-fw --label esp-pluto-sdr-five-targets --force --skip-rea
# Copy only the pinned Fishball7020 fsbl.elf files into the matching target/build/.
python rsa.py analyze fishball-datv --force
python rsa.py analyze tezuka-fw --force
python rsa.py compare tezuka-fw fishball7020-fpga-devkit fishball-datv libiio plutosdr-fw --label esp-pluto-sdr-five-targets
```

The first command attempt to copy binaries used paths relative to the wrong
working directory and failed; the corrected copies and analysis completed.
No bootloader was executed, modified or flashed.

## Failures and boundaries

All six REA objdump operations failed at provider launch. A direct shell
`/usr/bin/objdump -f` succeeds at ELF header inspection, but reports unknown
architecture; this host tool is not an ARM disassembler. Retrying REA with
REA_OBJDUMP_COMMAND=/usr/bin/objdump also failed. The retry error JSON is retained.
Do not equate successful artifact inventory with decompilation or call-graph analysis.
Ghidra is still absent. No firmware region/unpacking claim is made.

Native network-only libiio configure was attempted after installing CMake 4.4.4
and Ninja 1.13.2. With ZSTD disabled, CMake rejects the asynchronous IIOD client
configuration. With it enabled, ZSTD and libxml2 development dependencies are
missing. Both configure outputs are retained. No native or ESP build succeeded.
This is a baseline build probe, not evidence that an ESP port is infeasible.

The primary Fishball devkit and ADI baseline have no selected build-directory
binary in this pass. Their source metrics do not cover recursively fetched
kernel/HDL/Buildroot source. No installed factory image was available for comparison.

## Next gates

Capture the installed image version/target and actual IIO context; inspect the
corresponding firmware rather than guessing from repo HEAD. Provision native
ZSTD/libxml2 development libraries for a reference-client build. Add an ARM-aware
binary provider if deeper FSBL analysis is useful; prioritize IIOD and sample
transport over bootloader disassembly for driver development. Hardware USB,
IQ continuity, rate limits and RF verification remain pending.

No merge and no generated REA archive publication was performed.
