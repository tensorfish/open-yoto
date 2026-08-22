#include "yoto_vfs.h"

#include "esp_log.h"
#include "esp_vfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "YOTO_VFS";

extern const unsigned char stock_welcome_start[]
    asm("_binary_stock_welcome_m4a_start");
extern const unsigned char stock_welcome_end[]
    asm("_binary_stock_welcome_m4a_end");

#define YOTO_VFS_LOCAL_PATH "/sounds/welcome"
#define YOTO_VFS_MAX_OPEN_FILES 2

typedef struct
{
    bool registered;
    bool open[YOTO_VFS_MAX_OPEN_FILES];
    size_t offset[YOTO_VFS_MAX_OPEN_FILES];
} yoto_vfs_context_t;

static yoto_vfs_context_t s_ctx;

static bool valid_path(const char *path)
{
    return path != NULL && strcmp(path, YOTO_VFS_LOCAL_PATH) == 0;
}

static bool valid_fd(yoto_vfs_context_t *ctx, int fd)
{
    return fd >= 0 && fd < YOTO_VFS_MAX_OPEN_FILES && ctx->open[fd];
}

const unsigned char *yoto_vfs_welcome_data(size_t *size)
{
    if (size != NULL)
    {
        *size = (size_t)(stock_welcome_end - stock_welcome_start);
    }
    return stock_welcome_start;
}

static int yoto_open(void *opaque, const char *path, int flags, int mode)
{
    yoto_vfs_context_t *ctx = opaque;
    (void)mode;

    if (!valid_path(path))
    {
        errno = ENOENT;
        return -1;
    }
    if ((flags & O_ACCMODE) != O_RDONLY)
    {
        errno = EROFS;
        return -1;
    }
    for (int fd = 0; fd < YOTO_VFS_MAX_OPEN_FILES; fd++)
    {
        if (!ctx->open[fd])
        {
            ctx->open[fd] = true;
            ctx->offset[fd] = 0;
            ESP_LOGI(TAG, "opening read-only sound %s", path);
            return fd;
        }
    }
    errno = EMFILE;
    return -1;
}

static int yoto_close(void *opaque, int fd)
{
    yoto_vfs_context_t *ctx = opaque;

    if (!valid_fd(ctx, fd))
    {
        errno = EBADF;
        return -1;
    }
    ctx->open[fd] = false;
    ctx->offset[fd] = 0;
    return 0;
}

static ssize_t yoto_read(void *opaque, int fd, void *dst, size_t size)
{
    yoto_vfs_context_t *ctx = opaque;
    size_t asset_size;
    const unsigned char *asset = yoto_vfs_welcome_data(&asset_size);

    if (!valid_fd(ctx, fd))
    {
        errno = EBADF;
        return -1;
    }
    if (ctx->offset[fd] >= asset_size)
    {
        return 0;
    }
    if (size > asset_size - ctx->offset[fd])
    {
        size = asset_size - ctx->offset[fd];
    }
    memcpy(dst, asset + ctx->offset[fd], size);
    ctx->offset[fd] += size;
    return (ssize_t)size;
}

static ssize_t yoto_pread(void *opaque, int fd, void *dst, size_t size,
                          off_t offset)
{
    yoto_vfs_context_t *ctx = opaque;
    size_t asset_size;
    const unsigned char *asset = yoto_vfs_welcome_data(&asset_size);

    if (!valid_fd(ctx, fd))
    {
        errno = EBADF;
        return -1;
    }
    if (offset < 0 || (size_t)offset >= asset_size)
    {
        return 0;
    }
    if (size > asset_size - (size_t)offset)
    {
        size = asset_size - (size_t)offset;
    }
    memcpy(dst, asset + offset, size);
    return (ssize_t)size;
}

static off_t yoto_lseek(void *opaque, int fd, off_t offset, int whence)
{
    yoto_vfs_context_t *ctx = opaque;
    size_t asset_size;
    off_t next;
    (void)yoto_vfs_welcome_data(&asset_size);

    if (!valid_fd(ctx, fd))
    {
        errno = EBADF;
        return -1;
    }
    if (whence == SEEK_SET)
    {
        next = offset;
    }
    else if (whence == SEEK_CUR)
    {
        next = (off_t)ctx->offset[fd] + offset;
    }
    else if (whence == SEEK_END)
    {
        next = (off_t)asset_size + offset;
    }
    else
    {
        errno = EINVAL;
        return -1;
    }
    if (next < 0 || (size_t)next > asset_size)
    {
        errno = EINVAL;
        return -1;
    }
    ctx->offset[fd] = (size_t)next;
    return next;
}

static int fill_stat(struct stat *st)
{
    size_t size;
    (void)yoto_vfs_welcome_data(&size);
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0444;
    st->st_size = (off_t)size;
    return 0;
}

static int yoto_fstat(void *opaque, int fd, struct stat *st)
{
    yoto_vfs_context_t *ctx = opaque;

    if (!valid_fd(ctx, fd))
    {
        errno = EBADF;
        return -1;
    }
    return fill_stat(st);
}

#ifdef CONFIG_VFS_SUPPORT_DIR
static int yoto_stat(void *opaque, const char *path, struct stat *st)
{
    (void)opaque;
    if (!valid_path(path))
    {
        errno = ENOENT;
        return -1;
    }
    return fill_stat(st);
}
#endif

esp_err_t yoto_vfs_init(void)
{
    if (s_ctx.registered)
    {
        return ESP_OK;
    }

    const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_READONLY_FS,
        .open_p = yoto_open,
        .close_p = yoto_close,
        .read_p = yoto_read,
        .pread_p = yoto_pread,
        .lseek_p = yoto_lseek,
        .fstat_p = yoto_fstat,
#ifdef CONFIG_VFS_SUPPORT_DIR
        .stat_p = yoto_stat,
#endif
    };
    esp_err_t err = esp_vfs_register("/system", &vfs, &s_ctx);
    if (err == ESP_OK)
    {
        s_ctx.registered = true;
        ESP_LOGI(TAG, "registered /system (%u-byte welcome asset)",
                 (unsigned)(stock_welcome_end - stock_welcome_start));
    }
    return err;
}
