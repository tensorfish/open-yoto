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
    -o /tmp/encoder_hold_test \
    encoder_hold_test.c

cc -std=c11 -Wall -Wextra -Werror \
    -I stubs \
    -o /tmp/panel_clear_test \
    panel_clear_test.c

cc -std=c11 -Wall -Wextra -Werror \
    -I stubs -I ../../components/board \
    -DCONFIG_BOARD_REV_04 \
    -o /tmp/iox_wake_rev04_test \
    iox_wake_test.c

cc -std=c11 -Wall -Wextra -Werror \
    -I stubs -I ../../components/board \
    -o /tmp/iox_wake_rev05_test \
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
    -I stubs -I ../../components/board \
    -o /tmp/battery_charge_rev04_test \
    -DCONFIG_BOARD_REV_04 \
    battery_charge_test.c

cc -std=c11 -Wall -Wextra -Werror \
    -I stubs -I ../../components/board \
    -o /tmp/battery_charge_rev05_test \
    battery_charge_test.c

cc -std=c11 -Wall -Wextra -Werror \
    -I stubs -I ../../components/board \
    -DCONFIG_BOARD_REV_04 \
    -o /tmp/board_i2c_rev04_test \
    board_i2c_test.c ../../components/board/board_i2c.c

cc -std=c11 -Wall -Wextra -Werror \
    -I stubs -I ../../components/board \
    -o /tmp/board_i2c_rev05_test \
    board_i2c_test.c ../../components/board/board_i2c.c

cc -std=c11 -Wall -Wextra -Werror \
    -I nfc_stubs -I stubs \
    -o /tmp/nfc_recovery_test \
    nfc_recovery_test.c

/tmp/volume_overlay_test
echo "PASS: volume overlay geometry (rev04)"

/tmp/volume_overlay_rev05_test
echo "PASS: volume overlay geometry (rev05)"

/tmp/encoder_hold_test
echo "PASS: encoder button hold timing"

/tmp/panel_clear_test
echo "PASS: panel clear elision (rev04)"

/tmp/iox_wake_rev04_test
echo "PASS: IOX power and wake control (rev04)"

/tmp/iox_wake_rev05_test
echo "PASS: IOX power and wake control (rev05)"

/tmp/display_state_test
echo "PASS: display state machine (rev04)"

/tmp/display_state_rev05_test
echo "PASS: display state machine (rev05)"

/tmp/battery_charge_rev04_test
echo "PASS: charger hot-plug control (rev04)"

/tmp/battery_charge_rev05_test
echo "PASS: charger hot-plug control (rev05)"

/tmp/board_i2c_rev04_test
/tmp/board_i2c_rev04_test remove
/tmp/board_i2c_rev04_test delete
echo "PASS: shared I2C bus lifecycle (rev04)"

/tmp/board_i2c_rev05_test
/tmp/board_i2c_rev05_test remove
/tmp/board_i2c_rev05_test delete
echo "PASS: shared I2C bus lifecycle (rev05)"

/tmp/nfc_recovery_test
echo "PASS: NFC UART synchronization recovery"

echo "PASS: all host firmware tests"
