/*
 * encoder.h — Rotary encoders (PCNT quadrature) + push buttons + power button.
 *
 * Two detented rotary encoders are decoded in 4X quadrature with the ESP32
 * pulse counter, and their push buttons plus the power button are read from
 * the PI4IOE5V6416 IO expander and debounced. A FreeRTOS task delivers:
 *   - a TURN event per detent (positive/negative delta),
 *   - a SHORT_PRESS event on button release before its hold threshold,
 *   - a LONG_PRESS event after 800 ms for the left knob or 3000 ms for the
 *     right knob and dedicated power button.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Identifiers passed to the callback. ENCODER_ID_POWER is the power button
 * (no encoder attached, so its delta is always 0). */
enum
{
    ENCODER_ID_0 = 0,
    ENCODER_ID_1 = 1,
    ENCODER_ID_POWER = 2,
};

/* Button/encoder event kinds delivered to the callback. */
typedef enum
{
    ENCODER_EVT_TURN = 1,       /* rotation; delta is a signed detent count  */
    ENCODER_EVT_SHORT_PRESS,    /* released before the long-press threshold  */
    ENCODER_EVT_LONG_PRESS,     /* held past the long-press threshold        */
} encoder_event_t;

/** Application callback, invoked only from the encoder task. */
typedef void (*encoder_cb_t)(int encoder_id, int delta, encoder_event_t event);

/**
 * Initialize the encoders, the button debounce, and the delivery task.
 * Requires iox_init() to have run first.
 *
 * @return ESP_OK on success, or the first PCNT/queue/task error encountered.
 */
esp_err_t encoder_init(void);

/**
 * Register the application callback (may be NULL to disable delivery).
 *
 * @param cb Callback receiving (encoder_id, delta, event). delta is nonzero
 *           only for ENCODER_EVT_TURN.
 */
void encoder_register_cb(encoder_cb_t cb);
