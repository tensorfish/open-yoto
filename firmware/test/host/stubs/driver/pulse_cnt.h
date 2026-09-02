#ifndef HOST_STUB_DRIVER_PULSE_CNT_H
#define HOST_STUB_DRIVER_PULSE_CNT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void *pcnt_unit_handle_t;
typedef void *pcnt_channel_handle_t;

typedef struct {
    int low_limit;
    int high_limit;
} pcnt_unit_config_t;

typedef struct {
    uint32_t max_glitch_ns;
} pcnt_glitch_filter_config_t;

typedef struct {
    int edge_gpio_num;
    int level_gpio_num;
} pcnt_chan_config_t;

typedef struct {
    int watch_point_value;
} pcnt_watch_event_data_t;

typedef bool (*pcnt_watch_cb_t)(pcnt_unit_handle_t unit,
                                const pcnt_watch_event_data_t *edata,
                                void *user_ctx);

typedef struct {
    pcnt_watch_cb_t on_reach;
} pcnt_event_callbacks_t;

enum {
    PCNT_CHANNEL_EDGE_ACTION_DECREASE,
    PCNT_CHANNEL_EDGE_ACTION_INCREASE,
    PCNT_CHANNEL_LEVEL_ACTION_KEEP,
    PCNT_CHANNEL_LEVEL_ACTION_INVERSE,
};

esp_err_t pcnt_new_unit(const pcnt_unit_config_t *config,
                        pcnt_unit_handle_t *unit);
esp_err_t pcnt_unit_set_glitch_filter(
    pcnt_unit_handle_t unit, const pcnt_glitch_filter_config_t *config);
esp_err_t pcnt_new_channel(pcnt_unit_handle_t unit,
                           const pcnt_chan_config_t *config,
                           pcnt_channel_handle_t *channel);
esp_err_t pcnt_channel_set_edge_action(pcnt_channel_handle_t channel,
                                       int positive, int negative);
esp_err_t pcnt_channel_set_level_action(pcnt_channel_handle_t channel,
                                        int high, int low);
esp_err_t pcnt_unit_add_watch_point(pcnt_unit_handle_t unit, int point);
esp_err_t pcnt_unit_register_event_callbacks(
    pcnt_unit_handle_t unit, const pcnt_event_callbacks_t *callbacks,
    void *user_ctx);
esp_err_t pcnt_unit_enable(pcnt_unit_handle_t unit);
esp_err_t pcnt_unit_clear_count(pcnt_unit_handle_t unit);
esp_err_t pcnt_unit_start(pcnt_unit_handle_t unit);

#endif /* HOST_STUB_DRIVER_PULSE_CNT_H */
