#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

cc -std=c11 -Wall -Wextra -Werror \
    -I stubs \
    -o /tmp/volume_overlay_test \
    volume_overlay_test.c

cc -std=c11 -Wall -Wextra -Werror \
    -I stubs \
    -o /tmp/volume_overlay_rev05_test \
    volume_overlay_rev05_test.c

/tmp/volume_overlay_test
echo "PASS: volume overlay geometry (rev04)"

/tmp/volume_overlay_rev05_test
echo "PASS: volume overlay geometry (rev05)"

echo "PASS: all host display tests"
