/*
 * app_main.c — Yoto replacement firmware: boot + init sequence.
 *
 * Brings up the board in the same order the factory firmware does, then loops.
 * Wi-Fi is intentionally NOT initialized (the hidden upload mode is a later,
 * opt-in feature).
 */
#include "board_pins.h"

#include "battery.h"
#include "display.h"
#include "nfc.h"
#include "audio.h"
#include "iox.h"

#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "yoto firmware starting");

    /* Non-volatile storage (config, OTA state). */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* IO expander first — buttons, display CS, amp enable and power rails all
     * hang off it, and it brings up the shared I2C bus used by everything. */
    ESP_ERROR_CHECK(iox_init());

    /* Peripheral init. Each returns ESP_OK on success and logs its state. */
    ESP_ERROR_CHECK(battery_init());
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(nfc_init());
    ESP_ERROR_CHECK(audio_init());

    ESP_LOGI(TAG, "boot complete — entering main loop");

    /* Main loop: poll NFC, buttons, and play/display per the mapping. */
    while (1) {
        /* TODO: scan NFC -> look up mapping.json -> play sound + show image. */
        /* TODO: poll buttons -> encoder volume/selection + all-buttons chord
         *      toggles the (later) upload mode. */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
