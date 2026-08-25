/*
 * Host-test stub for encoder.h. Mirrors the event types, IDs and prototypes
 * app_main.c references; the host test provides no-op definitions.
 */
#ifndef HOST_STUB_ENCODER_H
#define HOST_STUB_ENCODER_H

#include <stdint.h>

#include "esp_err.h"

enum {
    ENCODER_ID_0 = 0,
    ENCODER_ID_1 = 1,
    ENCODER_ID_POWER = 2,
};

typedef enum {
    ENCODER_EVT_TURN = 1,
    ENCODER_EVT_SHORT_PRESS,
    ENCODER_EVT_LONG_PRESS,
} encoder_event_t;

typedef void (*encoder_cb_t)(int encoder_id, int delta, encoder_event_t event);

esp_err_t encoder_init(void);
void encoder_register_cb(encoder_cb_t cb);

#endif /* HOST_STUB_ENCODER_H */
