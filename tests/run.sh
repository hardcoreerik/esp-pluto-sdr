#!/bin/sh
# Usage: tests/run.sh [radio-ip]   (WSL/Linux). Pure tests always; live receive-only smoke test when an address is given.
set -e
cd "$(dirname "$0")/.."
gcc -std=c11 -Wall -Wextra -Werror -Iinclude -o /tmp/pluto_host_test tests/host_test.c src/pluto_iiod.c src/pluto_iq.c src/pluto_plan.c
/tmp/pluto_host_test "$@"
