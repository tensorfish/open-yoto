/*
 * Host-test stub for admin.h. Mirrors the callback types and prototypes
 * app_main.c references; the host test provides no-op/false definitions.
 */
#ifndef HOST_STUB_ADMIN_H
#define HOST_STUB_ADMIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ADMIN_ACCESS_CODE_LEN 6

typedef void (*admin_code_cb_t)(
    const char code[ADMIN_ACCESS_CODE_LEN + 1]);
typedef esp_err_t (*admin_path_cb_t)(const char *absolute_sd_path);
typedef esp_err_t (*admin_action_cb_t)(void);
typedef esp_err_t (*admin_card_write_cb_t)(
    const char *url, const uint8_t *expected_uid, uint8_t uid_len);

esp_err_t admin_start(char *code_out, size_t code_size);
esp_err_t admin_stop(void);
bool admin_is_active(void);
void admin_set_last_card(const uint8_t *uid, uint8_t uid_len,
                         const char *url);
void admin_set_code_callback(admin_code_cb_t cb);
void admin_set_card_write_callback(admin_card_write_cb_t cb);
void admin_set_path_callbacks(admin_path_cb_t play_sound,
                              admin_path_cb_t display_image,
                              admin_action_cb_t stop_sound,
                              admin_action_cb_t pause_sound,
                              admin_action_cb_t resume_sound,
                              admin_action_cb_t clear_display);

#endif /* HOST_STUB_ADMIN_H */
