/*
 * Host-test stub for yoto_vfs.h. The host test provides a no-op yoto_vfs_init().
 */
#ifndef HOST_STUB_YOTO_VFS_H
#define HOST_STUB_YOTO_VFS_H

#include "esp_err.h"

#define YOTO_WELCOME_PATH "/system/sounds/welcome"

esp_err_t yoto_vfs_init(void);

#endif /* HOST_STUB_YOTO_VFS_H */
