/*
 * content.h — SD-card content store: library.json index + media directory.
 *
 * Owns the FatFS SDMMC (1-bit) mount at /sdcard and library.json, which ties
 * an NFC URI to media stored under /sdcard/media/. Playlist entries use:
 *
 *     {"cards":[{"url":"...","name":"...","tracks":["media/a.mp3"],
 *       "track_images":["media/a.img"],"image":"media/cover.img"}]}
 * `track_images` is parallel to `tracks`; an empty item uses the optional
 * card-cover image. Legacy {"url","sound","image"} entries remain readable.
 * If present without library.json, the legacy mapping.json is migrated once.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

/** VFS mount point the content store owns. */
#define CONTENT_MOUNT_POINT "/sdcard"

/**
 * Mount the SD card at /sdcard (FatFS over SDMMC, 1-bit), ensure
 * /sdcard/media/ exists, and lazily load library.json. If library.json is
 * absent, a legacy /sdcard/mapping.json is migrated once and retained as a
 * fallback artifact. Idempotent: subsequent calls return ESP_OK without
 * remounting.
 *
 * @return ESP_OK on success, or an esp_err_t from the mount/IO layer.
 */
esp_err_t content_init(void);

/**
 * Look up a URL in library.json and copy the matched card's sound and image
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
 * Return the number of playable tracks for a card, or -1 if the URL is
 * unknown. A card with a "tracks" array uses its length; a card carrying only
 * a legacy "sound" string counts as one track.
 *
 * @param url URL key to match against cards[].url.
 * @return track count (>=0), or -1 if the URL is not in the catalog.
 */
int content_get_track_count(const char *url);

/**
 * Copy the index-th track's sound path into sound_path. If the card has a
 * "tracks" array, index selects within it; otherwise only index 0 returns the
 * legacy "sound" value.
 *
 * @param url        URL key.
 * @param index      zero-based track index.
 * @param sound_path output buffer (or NULL to skip).
 * @param sp         size of sound_path in bytes.
 * @return ESP_OK if the track exists, ESP_ERR_NOT_FOUND if the URL is unknown
 *         or index is out of range, ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t content_get_track(const char *url, int index,
                            char *sound_path, size_t sp);

/**
 * Add or replace a legacy single-track catalog entry. Names may be bare media
 * names or safe content-relative "media/..." paths. A card already carrying
 * url is replaced.
 *
 * @param url         URL key.
 * @param sound_name  sound media name/path.
 * @param image_name  image media name/path, or "" for no image.
 * @return ESP_OK on success, or an esp_err_t from the IO layer.
 */
esp_err_t content_add(const char *url, const char *sound_name,
                      const char *image_name);
/**
 * Add or replace a playlist card: an ordered list of audio tracks, each with
 * an optional per-track image, plus an optional cover image. Entries may be
 * bare media names or safe content-relative "media/..." paths, including
 * nested media folders; persisted entries always use "media/...".
 *
 * @param url          URL key.
 * @param name         optional display name; empty/NULL defaults to url.
 * @param tracks       array of n sound media names/paths.
 * @param track_images array of n image media names/paths; each entry may be
 *                     "" for a track that falls back to the cover.
 * @param n            number of tracks (and track_images entries).
 * @param cover_image  cover media name/path, or NULL/"" for none.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an unsafe/oversize path,
 *         ESP_ERR_NO_MEM on allocation failure, or ESP_FAIL on file IO.
 */
esp_err_t content_add_playlist(const char *url,
                               const char *name,
                               const char *const tracks[],
                               const char *const track_images[],
                               int n,
                               const char *cover_image);

/**
 * Copy the image media path for a card's index-th track into image_path.
 * Falls back to the card's cover image when the track has no image.
 *
 * @param url        URL key.
 * @param index      zero-based track index.
 * @param image_path output buffer (or NULL to skip).
 * @param ip         size of image_path in bytes.
 * @return ESP_OK if a non-empty path was written, ESP_ERR_NOT_FOUND if no
 *         image is available, ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t content_get_track_image(const char *url, int index,
                                  char *image_path, size_t ip);

/**
 * Remove the catalog entry whose URL matches url and persist the change.
 *
 * @param url URL key to remove.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no card matches,
 *         ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t content_delete(const char *url);

/**
 * Remove every catalog entry and persist the empty catalog atomically.
 *
 * @return ESP_OK on success, or an esp_err_t from index loading, allocation,
 *         or persistence. The in-memory catalog is restored if persistence
 *         fails.
 */
esp_err_t content_delete_all(void);

/**
 * Allocate the library catalog as a compact JSON array on first request.
 *
 * The caller owns `*out` and must release it with cJSON_free(). Keeping
 * allocation ownership avoids a second fixed-size copy in the HTTP server.
 */
esp_err_t content_list_json(char **out);
