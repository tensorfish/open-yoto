/*
 * test_main.c — boot bring-up test firmware.
 *
 * Minimal hardware smoke test: bring up the IO expanders, fuel gauge, the
 * 16x16 LED matrix, and the I2S/ES8156 audio path, then permanently display a
 * border + cross on the panel and emit a repeating beep on the headphone DAC.
 * No NFC, SD card, or encoder — this only proves power, I2C, SPI/display, and
 * audio are alive.
 */
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "iox.h"
#include "battery.h"
#include "ht16d35x.h"
#include "audio.h"

static const char *TAG = "test";

/* Draw a border + diagonal cross so a working panel is unmistakable. */
static void draw_test_image(void)
{
    int i;

    ht16d35x_clear();

    for (i = 0; i < 16; i++)
    {
        ht16d35x_set_pixel(i, 0, true);
        ht16d35x_set_pixel(i, 15, true);
        ht16d35x_set_pixel(0, i, true);
        ht16d35x_set_pixel(15, i, true);
    }
    for (i = 0; i < 16; i++)
    {
        ht16d35x_set_pixel(i, i, true);
        ht16d35x_set_pixel(i, 15 - i, true);
    }

    ht16d35x_flush();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(iox_init());
    ESP_ERROR_CHECK(battery_init());
    ESP_ERROR_CHECK(ht16d35x_init());
    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(audio_set_volume(50));

    ESP_LOGI(TAG, "boot test: display + 1 kHz beep");

    draw_test_image();

    /* Blocks forever, beeping on/off. */
    audio_play_tone(1000);
}
