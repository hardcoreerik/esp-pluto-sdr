#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude tests/test_driver.c src/esp_pluto_sdr.c -o /tmp/pluto-test
ASAN_OPTIONS=detect_leaks=0 /tmp/pluto-test
cc -std=c11 -Wall -Wextra -Werror -Iinclude tests/tcp_probe.c src/esp_pluto_tcp.c src/esp_pluto_sdr.c -o /tmp/pluto-tcp-test
python3 tests/test_tcp.py
