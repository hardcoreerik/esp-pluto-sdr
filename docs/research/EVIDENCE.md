# Evidence, execution log and limits

Snapshot 2026-11-02. sources.json records full Git commits and canonical upstream URLs.
The REA-Results INDEX.md, index.json and graph.json were read; no Pluto, Fishball,
Tezuka or libiio match was present in those indexes at this snapshot.
RSA-Sandbox/AGENTS.md and REA-Results/AGENTS.md were read through the connector.
LOCAL_RSA_REA_SESSION_PROMPT.md at the RSA repository root returned 404.

## What actually ran

The private RSA checkout could not be cloned through unauthenticated shell Git.
The Python RSA modules, config and templates were retrieved through the connected
GitHub API into this sandbox. No changes were made to the private RSA source repo.

1. `python rsa.py doctor`: initially failed because lizard was missing.
2. Installed lizard 1.25.0 and rich 15.0.0; final doctor returned `Status: ready`.
3. Public source checkouts were imported through `rsa.py import local:<path> --name <slug>`.
   Inputs remained read-only; provenance joins the local import to sources.json.
4. `python rsa.py compare tezuka-fw fishball7020-fpga-devkit fishball-datv libiio --label esp-pluto-sdr-firmware --skip-rea` completed.
5. `python rsa.py analyze plutosdr-fw --skip-rea` was run for the ADI reference.
6. `python tools/selftest/test_dsp.py` completed successfully; output retained.
7. `timeout 40 python tools/selftest/test_safety.py` completed successfully:
   44/44 upstream offline safety checks. Output retained. No radio was contacted.

The first comparison lacked downloaded templates and failed during report rendering.
After retrieving the templates, the comparison completed. An exploratory
`unittest discover` invocation was interrupted; these upstream files are scripts,
and test_dsp.py under unittest reports zero framework tests even though its
top-level checks execute. The direct script invocations above are the evidence.

RSA uses builtin approximate LOC counters because scc/cloc is absent. Function
metrics use lizard. Shallow input history limits activity/history metrics;
source-tree sizes are not full firmware sizes or performance measurements.
The original failed rendering left cached metrics; the final generated comparison
exists, but some single-target cached report files may not have been regenerated.

## Not executed

- No proprietary toolchain/IP analysis, firmware flashing, RF transmission or board writes.
- No full Buildroot/Linux/U-Boot/FPGA rebuild; dependency submodules remain uninitialized.
- No ESP-IDF cross-build or runnable ESP driver yet.
- No USB descriptor capture, RNDIS packet capture, desktop IQ baseline or P4 throughput test.
- No REA/Ghidra binary analysis: REA, Ghidra and binary-region tools are absent;
  no candidate firmware binary was selected. `--skip-rea` is explicit.
- No `rsa publish` execution or rewrite of REA's generated indexes/targets.
- No merge or update to main.

## Conclusions that must stay provisional

The intended board identity, exact factory image, FPGA implementation, current
USB mode, sustained throughput, loss/overflow observability, dual-RX quality and
TX stop guarantees are pending physical validation. Upstream measurements in
firmware documentation remain upstream-reported, not reproduced here.

## Reproduction

Use sources.json to clone each upstream at its full commit, then:

```sh
python rsa.py doctor
python rsa.py import local:/path/to/tezuka_fw --name tezuka-fw
python rsa.py import local:/path/to/fishball7020-fpga-devkit --name fishball7020-fpga-devkit
python rsa.py import local:/path/to/Fishball_DATV --name fishball-datv
python rsa.py import local:/path/to/libiio --name libiio
python rsa.py compare tezuka-fw fishball7020-fpga-devkit fishball-datv libiio --label esp-pluto-sdr-firmware --skip-rea
```

Review results, add analyst notes in the private RSA workspace, commit there,
then run `python rsa.py publish --dry-run`. Generated REA publication remains
separate from this project's research branch and must not be described as done.
