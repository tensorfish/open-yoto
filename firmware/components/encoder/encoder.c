/*
 * encoder.c — Rotary encoders (PCNT quadrature) + push buttons + power button.
 *
 * Two detented rotary encoders are decoded in 4X quadrature with the ESP32
 * pulse counter (one PCNT unit per encoder, two channels each). Their push
 * buttons and the power button are read from the PI4IOE5V6416 IO expander,
 * hysteresis-debounced, and classified into short vs long presses.
 *
 * Each detent spans ENCODER_COUNTS_PER_DETENT quadrature transitions; a watch
 * point is armed at +-ENCODER_COUNTS_PER_DETENT, the ISR clears the counter
 * and posts a signed one-detent event, and a FreeRTOS task drains the queue,
 * polls the buttons, and invokes the application callback.
 */
#include "encoder.h"

#include "board_pins.h"
#include "iox.h"

#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "encoder";

/* A detented quadrature encoder advances ENCODER_COUNTS_PER_DETENT transitions
 * per mechanical click when decoded at 4X resolution. */
#define ENCODER_COUNTS_PER_DETENT    4
#define ENCODER_MAX                  2

/* Buttons: the two encoder push buttons plus the power button. */
#define ENCODER_BTN_COUNT            3

/* PCNT counter limits. The counter is cleared at every detent, so these values
 * only need headroom for a single fast spin plus the watch-point margin. */
#define ENCODER_PCNT_LOW_LIMIT       (-100)
#define ENCODER_PCNT_HIGH_LIMIT      100

/* Quadrature input glitch filter width (nanoseconds). */
#define ENCODER_GLITCH_FILTER_NS     1000

/* Push buttons are wired active-low through the IO expander (pressed == 0). */
#define ENCODER_BTN_ACTIVE_LOW       1
#define ENCODER_BTN_POLL_MS          5
#define ENCODER_BTN_DEBOUNCE_MS      30
#define ENCODER_BTN_SAMPLES          (ENCODER_BTN_DEBOUNCE_MS / ENCODER_BTN_POLL_MS)

/* A button held this long is classified as a long press. */
#define ENCODER_LONG_PRESS_MS        800
#define ENCODER_LONG_PRESS_TICKS     (ENCODER_LONG_PRESS_MS / ENCODER_BTN_POLL_MS)

/* Queue depth carrying encoder events from the ISR to the task. */
#define ENCODER_QUEUE_DEPTH          16

/* Task stack size in bytes (ESP-IDF FreeRTOS convention). */
#define ENCODER_TASK_STACK_BYTES     4096

/* Per-encoder PCNT resources: one unit, two channels (4X quadrature). */
typedef struct
{
    int                   id;
    pcnt_unit_handle_t    unit;
    pcnt_channel_handle_t chan_a;
    pcnt_channel_handle_t chan_b;
} encoder_instance_t;

/* ISR -> task event; delta is a signed number of detents. */
typedef struct
{
    int encoder_id;
    int delta;
} encoder_msg_t;

/* Button debounce + long-press state. */
typedef struct
{
    bool     pressed;     /* debounced pressed state                        */
    uint8_t  count;       /* hysteresis counter toward/away from pressed    */
    uint32_t held_ticks;  /* poll ticks held since the press was accepted   */
    bool     long_fired;  /* the long-press event was already delivered     */
} encoder_button_t;

static QueueHandle_t s_encoder_queue;
static TaskHandle_t  s_encoder_task;

static encoder_instance_t s_encoders[ENCODER_MAX];
static encoder_button_t   s_buttons[ENCODER_BTN_COUNT];

/* Application callback, invoked only from the encoder task. */
static encoder_cb_t s_callback;

/**
 * PCNT watch-point ISR callback. Runs in ISR context, so only ISR-safe
 * FreeRTOS/driver calls are permitted here.
 */
static bool encoder_on_reach(pcnt_unit_handle_t unit,
                             const pcnt_watch_event_data_t *edata,
                             void *user_ctx)
{
    (void)unit;
    encoder_instance_t *enc = (encoder_instance_t *)user_ctx;
    encoder_msg_t evt;
    BaseType_t higher_priority_task_woken = pdFALSE;

    /* Translate the raw quadrature count at the watch point into detents. */
    evt.encoder_id = enc->id;
    evt.delta = edata->watch_point_value / ENCODER_COUNTS_PER_DETENT;

    /* Restart the count so the next detent is measured from zero. */
    pcnt_unit_clear_count(unit);

    xQueueSendFromISR(s_encoder_queue, &evt, &higher_priority_task_woken);
    return (higher_priority_task_woken == pdTRUE);
}

/**
 * Read a debounced button's raw level, normalized to "true == pressed".
 *
 * @param[in] id ENCODER_ID_0/1 (encoder push) or ENCODER_ID_POWER.
 * @return true if the button is currently asserted.
 */
static bool encoder_button_read(int id)
{
    uint8_t pin;

    if (id == ENCODER_ID_POWER)
    {
        pin = IOX_BTN_POWER;
    }
    else
    {
        pin = (id == ENCODER_ID_0) ? IOX_BTN_ENC0_PUSH : IOX_BTN_ENC1_PUSH;
    }

    bool level = iox_get_pin(pin);

#if ENCODER_BTN_ACTIVE_LOW
    return !level;
#else
    return level;
#endif
}

/**
 * Poll and debounce all buttons; classify each press as short or long.
 *
 * A short press is reported when a button is released before the long-press
 * threshold; a long press is reported once a button has been held past the
 * threshold (delivered at most once per hold).
 */
static void encoder_poll_buttons(void)
{
    for (int id = 0; id < ENCODER_BTN_COUNT; id++)
    {
        encoder_button_t *btn = &s_buttons[id];
        bool raw = encoder_button_read(id);

        if (raw)
        {
            if (btn->count < ENCODER_BTN_SAMPLES)
            {
                btn->count++;
                if (btn->count == ENCODER_BTN_SAMPLES && !btn->pressed)
                {
                    btn->pressed = true;
                    btn->held_ticks = 0;
                    btn->long_fired = false;
                }
            }
            else
            {
                btn->held_ticks++;
                if (!btn->long_fired &&
                    btn->held_ticks >= ENCODER_LONG_PRESS_TICKS)
                {
                    btn->long_fired = true;
                    if (s_callback != NULL)
                    {
                        s_callback(id, 0, ENCODER_EVT_LONG_PRESS);
                    }
                }
            }
        }
        else if (btn->count > 0)
        {
            btn->count--;
            if (btn->count == 0 && btn->pressed)
            {
                btn->pressed = false;
                if (!btn->long_fired && s_callback != NULL)
                {
                    s_callback(id, 0, ENCODER_EVT_SHORT_PRESS);
                }
            }
        }
    }
}

/** FreeRTOS task: drains encoder events and polls the buttons each tick. */
static void encoder_task(void *arg)
{
    (void)arg;
    encoder_msg_t evt;

    for (;;)
    {
        /* Block briefly for an encoder event, then poll the buttons. */
        if (xQueueReceive(s_encoder_queue, &evt,
                          pdMS_TO_TICKS(ENCODER_BTN_POLL_MS)) == pdTRUE)
        {
            if (s_callback != NULL)
            {
                s_callback(evt.encoder_id, evt.delta, ENCODER_EVT_TURN);
            }
        }
        encoder_poll_buttons();
    }
}

/** Configure one encoder: PCNT unit, 4X channels, filter, watch points, ISR. */
static esp_err_t encoder_setup_unit(encoder_instance_t *enc, int id,
                                    int edge_gpio, int level_gpio)
{
    esp_err_t err;

    enc->id = id;

    pcnt_unit_config_t unit_config = {
        .low_limit = ENCODER_PCNT_LOW_LIMIT,
        .high_limit = ENCODER_PCNT_HIGH_LIMIT,
    };
    err = pcnt_new_unit(&unit_config, &enc->unit);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "encoder %d: pcnt_new_unit: %s",
                 enc->id, esp_err_to_name(err));
        return err;
    }

    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = ENCODER_GLITCH_FILTER_NS,
    };
    err = pcnt_unit_set_glitch_filter(enc->unit, &filter_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "encoder %d: pcnt_unit_set_glitch_filter: %s",
                 enc->id, esp_err_to_name(err));
        return err;
    }

    /* Channel A: count on A edges, direction selected by the B level. */
    pcnt_chan_config_t chan_a_config = {
        .edge_gpio_num = edge_gpio,
        .level_gpio_num = level_gpio,
    };
    err = pcnt_new_channel(enc->unit, &chan_a_config, &enc->chan_a);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "encoder %d: pcnt_new_channel(A): %s",
                 enc->id, esp_err_to_name(err));
        return err;
    }

    /* Channel B: count on B edges, direction selected by the A level. The two
     * channels together count all four edges of a quadrature cycle (4X). */
    pcnt_chan_config_t chan_b_config = {
        .edge_gpio_num = level_gpio,
        .level_gpio_num = edge_gpio,
    };
    err = pcnt_new_channel(enc->unit, &chan_b_config, &enc->chan_b);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "encoder %d: pcnt_new_channel(B): %s",
                 enc->id, esp_err_to_name(err));
        return err;
    }

    /* 4X quadrature decode (canonical ESP-IDF two-channel arrangement). */
    err = pcnt_channel_set_edge_action(enc->chan_a,
                                       PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    if (err != ESP_OK) return err;
    err = pcnt_channel_set_level_action(enc->chan_a,
                                        PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                        PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    if (err != ESP_OK) return err;
    err = pcnt_channel_set_edge_action(enc->chan_b,
                                       PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    if (err != ESP_OK) return err;
    err = pcnt_channel_set_level_action(enc->chan_b,
                                        PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                        PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    if (err != ESP_OK) return err;

    /* Arm a watch point at +-one detent. */
    err = pcnt_unit_add_watch_point(enc->unit, ENCODER_COUNTS_PER_DETENT);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "encoder %d: add watch point +%d: %s",
                 enc->id, ENCODER_COUNTS_PER_DETENT, esp_err_to_name(err));
        return err;
    }
    err = pcnt_unit_add_watch_point(enc->unit, -ENCODER_COUNTS_PER_DETENT);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "encoder %d: add watch point -%d: %s",
                 enc->id, ENCODER_COUNTS_PER_DETENT, esp_err_to_name(err));
        return err;
    }

    pcnt_event_callbacks_t cbs = {
        .on_reach = encoder_on_reach,
    };
    err = pcnt_unit_register_event_callbacks(enc->unit, &cbs, enc);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "encoder %d: register event callbacks: %s",
                 enc->id, esp_err_to_name(err));
        return err;
    }

    err = pcnt_unit_enable(enc->unit);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "encoder %d: pcnt_unit_enable: %s",
                 enc->id, esp_err_to_name(err));
        return err;
    }
    err = pcnt_unit_clear_count(enc->unit);
    if (err != ESP_OK) return err;
    err = pcnt_unit_start(enc->unit);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "encoder %d: pcnt_unit_start: %s",
                 enc->id, esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t encoder_init(void)
{
    esp_err_t err;

    if (s_encoder_queue != NULL)
    {
        ESP_LOGW(TAG, "already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_encoder_queue = xQueueCreate(ENCODER_QUEUE_DEPTH, sizeof(encoder_msg_t));
    if (s_encoder_queue == NULL)
    {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return ESP_ERR_NO_MEM;
    }

    err = encoder_setup_unit(&s_encoders[0], ENCODER_ID_0, PIN_ENC0_A, PIN_ENC0_B);
    if (err != ESP_OK) return err;
    err = encoder_setup_unit(&s_encoders[1], ENCODER_ID_1, PIN_ENC1_A, PIN_ENC1_B);
    if (err != ESP_OK) return err;

    if (xTaskCreate(encoder_task, "encoder", ENCODER_TASK_STACK_BYTES,
                    NULL, 5, &s_encoder_task) != pdPASS)
    {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "2 encoders + 3 buttons ready (4X, %d counts/detent, %d ms long-press)",
             ENCODER_COUNTS_PER_DETENT, ENCODER_LONG_PRESS_MS);
    return ESP_OK;
}

void encoder_register_cb(encoder_cb_t cb)
{
    s_callback = cb;
}
