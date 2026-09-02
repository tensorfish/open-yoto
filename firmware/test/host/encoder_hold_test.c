#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../components/encoder/encoder.c"

static int s_failures;
static TickType_t s_now;
static bool s_pin_levels[256];
static unsigned s_event_count[ENCODER_BTN_COUNT];
static encoder_event_t s_last_event[ENCODER_BTN_COUNT];

#define CHECK(cond, ...)                                      \
    do                                                        \
    {                                                         \
        if (!(cond))                                          \
        {                                                     \
            fprintf(stderr, "FAIL: ");                       \
            fprintf(stderr, __VA_ARGS__);                     \
            fprintf(stderr, "\n");                          \
            s_failures++;                                     \
        }                                                     \
    } while (0)

const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

bool iox_get_pin(uint8_t pin)
{
    return s_pin_levels[pin];
}

TickType_t xTaskGetTickCount(void)
{
    return s_now;
}

void vTaskDelay(TickType_t ticks)
{
    s_now += ticks;
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 4096;
}

BaseType_t xTaskCreate(void (*code)(void *), const char *name,
                       uint32_t stack_bytes, void *arg,
                       UBaseType_t priority, TaskHandle_t *handle)
{
    (void)code;
    (void)name;
    (void)stack_bytes;
    (void)arg;
    (void)priority;
    if (handle != NULL)
    {
        *handle = (TaskHandle_t)(uintptr_t)1;
    }
    return pdPASS;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    (void)length;
    (void)item_size;
    return (QueueHandle_t)(uintptr_t)1;
}

BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item,
                             BaseType_t *higher_priority_task_woken)
{
    (void)queue;
    (void)item;
    if (higher_priority_task_woken != NULL)
    {
        *higher_priority_task_woken = pdFALSE;
    }
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    (void)queue;
    (void)item;
    (void)wait;
    return pdFALSE;
}

esp_err_t pcnt_new_unit(const pcnt_unit_config_t *config,
                        pcnt_unit_handle_t *unit)
{
    (void)config;
    *unit = (pcnt_unit_handle_t)(uintptr_t)1;
    return ESP_OK;
}

esp_err_t pcnt_unit_set_glitch_filter(
    pcnt_unit_handle_t unit, const pcnt_glitch_filter_config_t *config)
{
    (void)unit;
    (void)config;
    return ESP_OK;
}

esp_err_t pcnt_new_channel(pcnt_unit_handle_t unit,
                           const pcnt_chan_config_t *config,
                           pcnt_channel_handle_t *channel)
{
    (void)unit;
    (void)config;
    *channel = (pcnt_channel_handle_t)(uintptr_t)1;
    return ESP_OK;
}

esp_err_t pcnt_channel_set_edge_action(pcnt_channel_handle_t channel,
                                       int positive, int negative)
{
    (void)channel;
    (void)positive;
    (void)negative;
    return ESP_OK;
}

esp_err_t pcnt_channel_set_level_action(pcnt_channel_handle_t channel,
                                        int high, int low)
{
    (void)channel;
    (void)high;
    (void)low;
    return ESP_OK;
}

esp_err_t pcnt_unit_add_watch_point(pcnt_unit_handle_t unit, int point)
{
    (void)unit;
    (void)point;
    return ESP_OK;
}

esp_err_t pcnt_unit_register_event_callbacks(
    pcnt_unit_handle_t unit, const pcnt_event_callbacks_t *callbacks,
    void *user_ctx)
{
    (void)unit;
    (void)callbacks;
    (void)user_ctx;
    return ESP_OK;
}

esp_err_t pcnt_unit_enable(pcnt_unit_handle_t unit)
{
    (void)unit;
    return ESP_OK;
}

esp_err_t pcnt_unit_clear_count(pcnt_unit_handle_t unit)
{
    (void)unit;
    return ESP_OK;
}

esp_err_t pcnt_unit_start(pcnt_unit_handle_t unit)
{
    (void)unit;
    return ESP_OK;
}

static void record_event(int encoder_id, int delta, encoder_event_t event)
{
    (void)delta;
    if (encoder_id >= 0 && encoder_id < ENCODER_BTN_COUNT)
    {
        s_event_count[encoder_id]++;
        s_last_event[encoder_id] = event;
    }
}

static uint8_t button_pin(int id)
{
    if (id == ENCODER_ID_0)
    {
        return IOX_BTN_ENC0_PUSH;
    }
    if (id == ENCODER_ID_1)
    {
        return IOX_BTN_ENC1_PUSH;
    }
    return IOX_BTN_POWER;
}

static void reset_buttons(void)
{
    memset(s_buttons, 0, sizeof(s_buttons));
    memset(s_event_count, 0, sizeof(s_event_count));
    memset(s_last_event, 0, sizeof(s_last_event));
    memset(s_pin_levels, 1, sizeof(s_pin_levels));
    s_now = 0;
    s_callback = record_event;
}

static TickType_t debounce_press(int id)
{
    s_pin_levels[button_pin(id)] = false;
    for (unsigned sample = 0; sample < ENCODER_BTN_SAMPLES; sample++)
    {
        encoder_poll_buttons();
        if (sample + 1 < ENCODER_BTN_SAMPLES)
        {
            s_now += ENCODER_BTN_POLL_MS;
        }
    }
    CHECK(s_buttons[id].pressed, "button %d did not debounce pressed", id);
    return s_buttons[id].pressed_at;
}

static void debounce_release(int id)
{
    s_pin_levels[button_pin(id)] = true;
    for (unsigned sample = 0; sample < ENCODER_BTN_SAMPLES; sample++)
    {
        encoder_poll_buttons();
        if (sample + 1 < ENCODER_BTN_SAMPLES)
        {
            s_now += ENCODER_BTN_POLL_MS;
        }
    }
    CHECK(!s_buttons[id].pressed, "button %d did not debounce released", id);
}

static void check_long_boundary(int id, TickType_t threshold)
{
    reset_buttons();
    TickType_t accepted = debounce_press(id);

    s_now = accepted + threshold - 1;
    encoder_poll_buttons();
    CHECK(s_event_count[id] == 0,
          "button %d fired before %u ms", id, (unsigned)threshold);

    s_now = accepted + threshold;
    encoder_poll_buttons();
    CHECK(s_event_count[id] == 1
          && s_last_event[id] == ENCODER_EVT_LONG_PRESS,
          "button %d did not fire one LONG_PRESS at %u ms",
          id, (unsigned)threshold);

    s_now += threshold;
    encoder_poll_buttons();
    CHECK(s_event_count[id] == 1,
          "button %d repeated LONG_PRESS during one hold", id);

    debounce_release(id);
    CHECK(s_event_count[id] == 1,
          "button %d emitted SHORT_PRESS after a long hold", id);
}

static void check_short_release(int id, TickType_t required_threshold)
{
    reset_buttons();
    TickType_t accepted = debounce_press(id);

    s_now = accepted + required_threshold - 1;
    debounce_release(id);
    CHECK(s_event_count[id] == 1
          && s_last_event[id] == ENCODER_EVT_SHORT_PRESS,
          "button %d release before %u ms did not emit SHORT_PRESS",
          id, (unsigned)required_threshold);
}

int main(void)
{
    check_long_boundary(ENCODER_ID_0, 800);
    check_long_boundary(ENCODER_ID_1, 3000);
    check_long_boundary(ENCODER_ID_POWER, 3000);
    check_short_release(ENCODER_ID_0, 800);
    check_short_release(ENCODER_ID_1, 3000);
    check_short_release(ENCODER_ID_POWER, 3000);

    if (s_failures != 0)
    {
        fprintf(stderr, "encoder hold test: %d assertion(s) failed\n",
                s_failures);
        return 1;
    }
    printf("encoder hold test: all assertions passed\n");
    return 0;
}
