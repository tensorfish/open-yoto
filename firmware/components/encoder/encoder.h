/*
 * encoder.h — Rotary encoders (PCNT quadrature) + push buttons (IO expander).
 *
 * Two detented rotary encoders are decoded in 4X quadrature with the ESP32
 * pulse counter (one PCNT unit per encoder), and their push buttons are read
 * from the PI4IOE5V6416 IO expander and debounced. A dedicated FreeRTOS task
 * delivers one callback per detent and one callback per button press.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Encoder identifiers passed to the registered callback. */
enum
{
    ENCODER_ID_0 = 0,
    ENCODER_ID_1 = 1,
    ENCODER_ID_MAX = 2,
};

/**
 * Initialize both rotary encoders (4X quadrature, glitch-filtered, one watch
 * point per detent) and start the task that decodes them and debounces the two
 * push buttons on the IO expander.
 *
 * Requires the IO expander bus to be up first (iox_init must have run).
 *
 * @return ESP_OK on success, or the first PCNT/queue/task error encountered.
 */
esp_err_t encoder_init(void);

/**
 * Register the application callback invoked from the encoder task.
 *
 * @param cb Callback receiving the encoder id, the number of detents turned
 *           since the last report (positive for one rotation direction,
 *           negative for the other; the absolute sign follows the A/B wiring
 *           and may be flipped by swapping the two pins), and whether this
 *           invocation is a push-button press (button == true). May be NULL
 *           to disable delivery.
 */
void encoder_register_cb(void (*cb)(int encoder_id, int delta, bool button));
