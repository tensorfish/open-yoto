#ifndef HOST_STUB_FREERTOS_QUEUE_H
#define HOST_STUB_FREERTOS_QUEUE_H

#include "freertos/FreeRTOS.h"

typedef void *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item,
                             BaseType_t *higher_priority_task_woken);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);

#endif /* HOST_STUB_FREERTOS_QUEUE_H */
