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
#include "driver/ledc.h"

#include "board_pins.h"
#include "iox.h"
#include "battery.h"
#include "ht16d35x.h"
#include "gc9306.h"
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
    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(audio_set_volume(50));

#ifdef CONFIG_BOARD_REV_04
    /* Rev #04 has a GC9306 TFT, not the HT16D35x LED matrix the #05 display
     * driver targets. Backlight (GPIO26) must be PWM-driven — the stock
     * firmware uses LEDC at 40 kHz (literal 0x9C40 @ 0x400d4fd4) and the
     * panel LED rail is AC-coupled (a plain GPIO high passes nothing; a
     * lower frequency is attenuated by the coupling cap and leaves the
     * display dim enough that colours wash out). Full duty + stock 40 kHz. */
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz         = 40000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_chan = {
        .gpio_num   = PIN_DISPLAY_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 255,   /* full brightness */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_chan));

    ESP_ERROR_CHECK(gc9306_init());
    ESP_ERROR_CHECK(gc9306_display_on());
    /* Four-stripe colour diagnostic: red, green, blue, white (18-bit RGB).
     * Reports the panel's actual byte-order/mode mapping. */
    ESP_ERROR_CHECK(gc9306_fill_rect(0, 0, 59, 319, 0xFC0000));
    ESP_ERROR_CHECK(gc9306_fill_rect(60, 0, 119, 319, 0x00FC00));
    ESP_ERROR_CHECK(gc9306_fill_rect(120, 0, 179, 319, 0x0000FC));
    ESP_ERROR_CHECK(gc9306_fill_rect(180, 0, 239, 319, 0xFCFCFC));

    /* Diagnostic: read back the IOX input ports. Bits 0-3 of port 0 are
     * cs/dc/reset (held high after the draw) + level convertor — if the
     * ET6416 register map matches the PI4IOE5V6416 driver, they read 1.
     * Bits 6 of port 1 is vinhold. Garbage here = register-map mismatch. */
    uint8_t p0 = 0, p1 = 0;
    if (iox_read_port(0, 0, &p0) == ESP_OK && iox_read_port(0, 1, &p1) == ESP_OK)
    {
        ESP_LOGI(TAG, "rev04 diag: IOX p0=0x%02x p1=0x%02x (want p0 bits0-3=1, p1 bit6=1)",
                 p0, p1);
    }
    else
    {
        ESP_LOGE(TAG, "rev04 diag: iox_read_port failed");
    }
    ESP_LOGI(TAG, "boot test: rev04 TFT pattern + 1 kHz beep");
#else
    ESP_ERROR_CHECK(ht16d35x_init());
    ESP_LOGI(TAG, "boot test: display + 1 kHz beep");
    draw_test_image();
#endif

    /* Blocks forever, beeping on/off. */
    audio_play_tone(1000);
}
