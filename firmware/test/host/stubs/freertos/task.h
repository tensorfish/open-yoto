/*
 * Host-test stub for FreeRTOS task.h. xTaskCreate records the entry point but
 * never runs it; vTaskDelay advances the virtual clock; xTaskGetTickCount reads
 * the test-controlled counter. Definitions live in the host test.
 */
#ifndef HOST_STUB_FREERTOS_TASK_H
#define HOST_STUB_FREERTOS_TASK_H

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;

BaseType_t xTaskCreate(void (*code)(void *), const char *name,
                       uint32_t stack_bytes, void *arg,
                       UBaseType_t priority, TaskHandle_t *handle);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
/* Reports the task's minimum free stack; the host test returns a fixed healthy
 * margin so stack_watch() never warns during the run. */
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);

#endif /* HOST_STUB_FREERTOS_TASK_H */
