/*
 * admin.h — Admin mode: SoftAP + HTTP web UI for offline content management.
 *
 * Brings up an open Wi-Fi SoftAP ("openyoto", channel 1) with a static
 * 192.168.4.1 address and an embedded HTTP server. A random 4-digit access
 * code gates every write operation (add/delete); reads are unauthenticated so
 * the landing page and content list always render.
 *
 * The code is surfaced through an optional callback (admin_set_code_callback)
 * so the application can draw it on the player display.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

/**
 * Callback invoked with a freshly generated access code. The application can
 * render it on the display or log it; it may be NULL to disable the hook.
 */
typedef void (*admin_code_cb_t)(uint16_t code);

/**
 * Enter admin mode.
 *
 * Requires that NVS and the content store (SD card + mapping.json) are
 * available; content_init() is invoked here to guarantee the latter.
 *
 * @param[out] code_out  Receives the 4-digit access code (may be NULL).
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already active, or the
 *         underlying esp_wifi / esp_http_server / content error.
 */
esp_err_t admin_start(uint16_t *code_out);

/**
 * Leave admin mode: stop the HTTP server, tear down the AP, and free the
 * AP netif. Idempotent.
 */
esp_err_t admin_stop(void);

/** Return true while admin mode is active. */
bool admin_is_active(void);

/** Register the callback used to display the access code (may be NULL). */
void admin_set_code_callback(admin_code_cb_t cb);
