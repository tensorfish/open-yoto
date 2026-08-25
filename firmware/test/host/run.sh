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

cc -std=c11 -Wall -Wextra -Werror \
    -I stubs \
    -o /tmp/panel_clear_test \
    panel_clear_test.c

cc -std=c11 -Wall -Wextra -Werror \
    -I stubs \
    -o /tmp/display_state_test \
    display_state_test.c

/tmp/volume_overlay_test
echo "PASS: volume overlay geometry (rev04)"

/tmp/volume_overlay_rev05_test
echo "PASS: volume overlay geometry (rev05)"

/tmp/panel_clear_test
echo "PASS: panel clear elision (rev04)"

/tmp/display_state_test
echo "PASS: display state machine (rev04)"

echo "PASS: all host display tests"
