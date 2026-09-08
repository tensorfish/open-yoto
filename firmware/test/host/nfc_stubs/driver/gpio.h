#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef int gpio_num_t;
typedef enum
{
    GPIO_MODE_INPUT = 1,
    GPIO_MODE_OUTPUT = 2,
} gpio_mode_t;

esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
esp_err_t gpio_reset_pin(gpio_num_t pin);
