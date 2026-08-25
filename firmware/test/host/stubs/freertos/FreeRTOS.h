/*
 * Host-test stub for FreeRTOS.h. The virtual clock is 1 tick == 1 ms, so
 * pdMS_TO_TICKS(x) is the identity and configTICK_RATE_HZ is 1000, matching
 * the tick rate the firmware assumes. The tick counter and all task/semaphore
 * primitives are defined by the host test.
 */
#ifndef HOST_STUB_FREERTOS_H
#define HOST_STUB_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define portMAX_DELAY ((TickType_t)0xffffffffUL)

#define pdTRUE ((BaseType_t)1)
#define pdPASS ((BaseType_t)1)

#endif /* HOST_STUB_FREERTOS_H */
