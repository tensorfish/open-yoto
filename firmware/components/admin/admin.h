/*
 * admin.h — On-demand SoftAP web administration and remote control.
 *
 * While admin mode is active, the open `openyoto` hotspot serves the UI at
 * 192.168.4.1. A random six-character alphanumeric code is exchanged for an
 * HttpOnly session cookie; every API and file read is authenticated.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ADMIN_ACCESS_CODE_LEN 6

/** Called whenever a fresh six-character access code is generated. */
typedef void (*admin_code_cb_t)(
    const char code[ADMIN_ACCESS_CODE_LEN + 1]);

/** Player-side callback for an authenticated SD path operation. */
typedef esp_err_t (*admin_path_cb_t)(const char *absolute_sd_path);

/** Player-side callback for an authenticated action without a path. */
typedef esp_err_t (*admin_action_cb_t)(void);

/** Player-side callback for writing a URL to one expected NFC UID. */
typedef esp_err_t (*admin_card_write_cb_t)(
    const char *url, const uint8_t *expected_uid, uint8_t uid_len);

/**
 * Enter admin mode by starting the SoftAP and HTTP server.
 *
 * content_init() is called idempotently so SD file and remote-control APIs are
 * available before the server accepts requests.
 *
 * @param[out] code_out Optional buffer receiving the NUL-terminated access
 *                      code. Must hold ADMIN_ACCESS_CODE_LEN + 1 bytes.
 * @param code_size Capacity of code_out, or zero when code_out is NULL.
 */
esp_err_t admin_start(char *code_out, size_t code_size);

/**
 * Leave admin mode: stop the HTTP server, tear down the AP, and free the
 * AP netif. Idempotent.
 */
esp_err_t admin_stop(void);

/** Return true while admin mode is active. */
bool admin_is_active(void);
/**
 * Record the most recently scanned card for the authenticated admin UI.
 * A valid UID with an empty URL represents a blank/unformatted card.
 * Pass NULL/zero to clear captured state.
 */
void admin_set_last_card(const uint8_t *uid, uint8_t uid_len,
                         const char *url);

/** Register the callback used to display the access code (may be NULL). */
void admin_set_code_callback(admin_code_cb_t cb);

/** Register the NFC URL-write callback (may be NULL). */
void admin_set_card_write_callback(admin_card_write_cb_t cb);

/** Install player callbacks used by the remote-control HTTP API. */
void admin_set_path_callbacks(admin_path_cb_t play_sound,
                              admin_path_cb_t display_image,
                              admin_action_cb_t stop_sound,
                              admin_action_cb_t pause_sound,
                              admin_action_cb_t resume_sound,
                              admin_action_cb_t clear_display);
