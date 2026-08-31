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
    -o /tmp/iox_wake_test \
    iox_wake_test.c

cc -std=c11 -Wall -Wextra -Werror \
    -I stubs \
    -o /tmp/display_state_test \
    display_state_test.c

cc -std=c11 -Wall -Wextra -Werror \
    -I stubs \
    -DTEST_BOARD_REV05 \
    -o /tmp/display_state_rev05_test \
    display_state_test.c

cc -std=c11 -Wall -Wextra -Werror \
    -I stubs \
    -o /tmp/battery_charge_rev04_test \
    -DCONFIG_BOARD_REV_04 \
    battery_charge_test.c

cc -std=c11 -Wall -Wextra -Werror \
    -I stubs \
    -o /tmp/battery_charge_rev05_test \
    battery_charge_test.c

/tmp/volume_overlay_test
echo "PASS: volume overlay geometry (rev04)"

/tmp/volume_overlay_rev05_test
echo "PASS: volume overlay geometry (rev05)"

/tmp/panel_clear_test
echo "PASS: panel clear elision (rev04)"

/tmp/iox_wake_test
echo "PASS: IOX power-button interrupt control (rev04)"

/tmp/display_state_test
echo "PASS: display state machine (rev04)"

/tmp/display_state_rev05_test
echo "PASS: display state machine (rev05)"

/tmp/battery_charge_rev04_test
echo "PASS: charger hot-plug control (rev04)"

/tmp/battery_charge_rev05_test
echo "PASS: charger hot-plug control (rev05)"

echo "PASS: all host firmware tests"
