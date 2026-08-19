/*
 * content.h — SD-card content store: mapping.json index + media directory.
 *
 * Owns the FatFS SDMMC (1-bit) mount at /sdcard and the mapping.json index
 * that ties a URL (read from an NFC tag) to a sound file and an image file
 * stored under /sdcard/media/. The mapping file has the shape:
 *
 *     {"cards":[{"url":"...","sound":"media/a.mp3","image":"media/a.png"}]}
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

/** VFS mount point the content store owns. */
#define CONTENT_MOUNT_POINT "/sdcard"

/**
 * Mount the SD card at /sdcard (FatFS over SDMMC, 1-bit), ensure
 * /sdcard/mapping.json and /sdcard/media/ exist, and load the mapping into
 * memory. Idempotent: subsequent calls return ESP_OK without remounting.
 *
 * @return ESP_OK on success, or an esp_err_t from the mount/IO layer.
 */
esp_err_t content_init(void);

/**
 * Look up a URL in mapping.json and copy the matched card's sound and image
 * paths (relative to /sdcard, e.g. "media/a.mp3") into the caller buffers.
 * Either output may be NULL to skip it.
 *
 * @param url         URL key to match against cards[].url.
 * @param sound_path  output buffer for the sound path (or NULL).
 * @param sp          size of sound_path in bytes.
 * @param image_path  output buffer for the image path (or NULL).
 * @param ip          size of image_path in bytes.
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no card matches,
 *         ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t content_lookup(const char *url, char *sound_path, size_t sp,
                         char *image_path, size_t ip);

/**
 * Add or replace a mapping entry. sound_name and image_name are media file
 * names (no directory) stored under /sdcard/media/; they are persisted as
 * "media/<name>". A card already carrying url is replaced.
 *
 * @param url         URL key.
 * @param sound_name  sound file name (no path).
 * @param image_name  image file name (no path).
 * @return ESP_OK on success, or an esp_err_t from the IO layer.
 */
esp_err_t content_add(const char *url, const char *sound_name,
                      const char *image_name);

/**
 * Remove the mapping entry whose URL matches url and persist the change.
 *
 * @param url URL key to remove.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no card matches,
 *         ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t content_delete(const char *url);

/**
 * Serialize the whole mapping as a JSON array of card objects into out.
 *
 * @param out  output buffer.
 * @param cap  output buffer capacity in bytes.
 * @return ESP_OK on success, ESP_ERR_INVALID_SIZE if out is too small,
 *         ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t content_list(char *out, size_t cap);
