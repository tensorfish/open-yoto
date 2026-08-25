/*
 * Host-test stub for iox.h. app_main.c only calls iox_init(); the host test
 * provides a no-op definition.
 */
#ifndef HOST_STUB_IOX_H
#define HOST_STUB_IOX_H

#include "esp_err.h"

esp_err_t iox_init(void);

#endif /* HOST_STUB_IOX_H */
