#ifndef HOST_STUB_DRIVER_RTC_IO_H
#define HOST_STUB_DRIVER_RTC_IO_H

#include "esp_err.h"
#define GPIO_NUM_12 12

esp_err_t rtc_gpio_isolate(int gpio_num);

#endif /* HOST_STUB_DRIVER_RTC_IO_H */
