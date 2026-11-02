# Target: libiio (`libiio`)

- **Origin:** local:/workspace/scratch/b99a64b46c2a/sources/libiio
- **Pinned commit:** `ddac2991667ee897363c8ecd45d1dc3f6c3a7608`
- **Analyzed as of:** 2026-11-02
- **Tools:** git git version 2.51.1, lizard 1.25.0, python 3.12.14, rsa 0.1.3

## Summary

| Metric | Value |
|---|---|
| name | libiio |
| origin | local:/workspace/scratch/b99a64b46c2a/sources/libiio |
| commit | ddac2991667e |
| license | LGPL-2.1 |
| primary_language | C |
| files | 418 |
| code_lines | 54,005 |
| code_lines_programming | 46,964 |
| comment_lines | 9,909 |
| comment_ratio | 0.18 |
| languages | 16 |
| functions | 2,085 |
| avg_ccn | 4.33 |
| p90_ccn | 10 |
| max_ccn | 342 |
| functions_ccn_gt_10 | 173 |
| functions_ccn_gt_20 | 46 |
| avg_function_nloc | 17.10 |
| dependency_manifests | 35 |
| unique_dependencies | 14 |
| external_components | 8 |
| test_files | 33 |
| doc_files | 159 |
| readme_lines | 90 |
| ci_workflows | 18 |
| commits | 1 |
| contributors | 1 |
| contributor_identities | 1 |
| first_commit | 2026-10-26 |
| last_commit | 2026-10-26 |
| commits_last_90d | 1 |
| active_authors_last_90d | 1 |
| github_stars | 654 |
| github_forks | 373 |
| github_open_issues_prs | 41 |
| binaries_found | 0 |
| elf_machine | n/a |
| elf_flash_estimate_bytes | n/a |
| elf_ram_static_bytes | n/a |
| elf_function_symbols | n/a |

## Languages

| Language | Files | Code | Comment |
|---|---|---|---|
| C | 109 | 32,576 | 4,656 |
| Python | 18 | 3,569 | 362 |
| Markdown | 34 | 3,420 | 509 |
| CMake | 35 | 3,209 | 397 |
| C Header | 34 | 3,103 | 2,681 |
| C# | 21 | 2,391 | 509 |
| YAML | 49 | 1,927 | 137 |
| reStructuredText | 86 | 1,605 | 272 |
| Shell | 14 | 890 | 181 |
| C++ Header | 1 | 519 | 137 |
| PowerShell | 5 | 329 | 27 |
| Kconfig | 7 | 300 | 14 |

## Most complex functions

| Function | Location | CCN | NLOC |
|---|---|---|---|
| main | utils/iio_attr.c:402 | 342 | 725 |
| main | utils/iio_rwdev.c:279 | 94 | 315 |
| main | utils/iio_stresstest.c:388 | 90 | 335 |
| translate_wsa_error_to_posix | utils-windows.h:30 | 79 | 141 |
| getopt_internal | deps/wingetopt/src/getopt.c:323 | 57 | 137 |
| rw_thd | iiod/ops.c:418 | 56 | 176 |
| open_client_sockets | deps/mdns/mdns.c:454 | 49 | 180 |
| main | utils/iio_info.c:248 | 49 | 170 |
| service_callback | deps/mdns/mdns.c:181 | 44 | 150 |
| main | examples/dummy-iiostream.c:226 | 43 | 170 |

Raw outputs: `../metrics/` (summary.json, loc_*.json, lizard_functions.csv, dependencies.json, binaries.json) and `../rea/`.

