#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef int uart_port_t;
#define UART_DATA_8_BITS 3
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_2 3
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE (-1)

typedef struct
{
    int baud_rate;
    int data_bits;
    int parity;
    int stop_bits;
    int flow_ctrl;
    int source_clk;
} uart_config_t;

esp_err_t uart_driver_install(uart_port_t port, int rx_size, int tx_size,
                               int queue_size, void *queue, int flags);
esp_err_t uart_driver_delete(uart_port_t port);
esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config);
esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts);
esp_err_t uart_flush_input(uart_port_t port);
int uart_write_bytes(uart_port_t port, const void *data, size_t len);
int uart_read_bytes(uart_port_t port, void *data, uint32_t len,
                    TickType_t timeout);
