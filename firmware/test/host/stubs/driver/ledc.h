/*
 * Host-test stub for ESP-IDF's driver/ledc.h. Provides the minimal types and
 * constants the display component's backlight PWM init references.
 */
#ifndef HOST_STUB_DRIVER_LEDC_H
#define HOST_STUB_DRIVER_LEDC_H

#include "esp_err.h"
#include <stdint.h>

typedef enum {
    LEDC_LOW_SPEED_MODE = 0
} ledc_mode_t;

typedef enum {
    LEDC_TIMER_0 = 0
} ledc_timer_t;

typedef enum {
    LEDC_TIMER_8_BIT = 8
} ledc_timer_bit_t;

typedef enum {
    LEDC_AUTO_CLK = 0
} ledc_clk_cfg_t;

typedef enum {
    LEDC_CHANNEL_0 = 0
} ledc_channel_t;

typedef enum {
    LEDC_INTR_DISABLE = 0
} ledc_intr_type_t;

typedef struct {
    ledc_mode_t speed_mode;
    ledc_timer_t timer_num;
    ledc_timer_bit_t duty_resolution;
    uint32_t freq_hz;
    ledc_clk_cfg_t clk_cfg;
} ledc_timer_config_t;

typedef struct {
    int gpio_num;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_intr_type_t intr_type;
    ledc_timer_t timer_sel;
    uint32_t duty;
    uint32_t hpoint;
} ledc_channel_config_t;

esp_err_t ledc_timer_config(const ledc_timer_config_t *config);
esp_err_t ledc_channel_config(const ledc_channel_config_t *config);

#endif /* HOST_STUB_DRIVER_LEDC_H */
