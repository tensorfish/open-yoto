/*
 * Host-test stub for FreeRTOS semphr.h. Mutexes and binary semaphores are
 * trivial fakes: take/give always succeed and creation returns a non-NULL
 * handle. Includes task.h so app_main.c (which only includes semphr.h) still
 * sees vTaskDelay / xTaskGetTickCount / xTaskCreate.
 */
#ifndef HOST_STUB_FREERTOS_SEMPHR_H
#define HOST_STUB_FREERTOS_SEMPHR_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef void *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);

#endif /* HOST_STUB_FREERTOS_SEMPHR_H */
