/*
 * app_main.c — Yoto replacement firmware: state machine.
 *
 * Boot -> battery check (low-battery art) -> normal mode (NFC scan -> play +
 * display) with encoders for volume/skip and buttons for pause. A "magic" NDEF
 * URL toggles admin mode (open softAP + web UI + 4-digit code).
 *
 * Wi-Fi/BT are NOT enabled in normal mode; only admin mode brings up the radio.
 */
#include <stdio.h>
#include <string.h>

#include "nvs_flash.h"
#include "esp_log.h"

#include "board_pins.h"
#include "iox.h"
#include "battery.h"
#include "ht16d35x.h"
#include "cr95hf.h"
#include "encoder.h"
#include "content.h"
#include "audio.h"
#include "admin.h"

static const char *TAG = "main";

/* The NDEF URL written to the "magic" admin card. */
#define MAGIC_URL "openyoto.local/admin"

/* Number of encoder detents per volume step. */
#define VOLUME_DELTA_PER_DETENT 5
#define VOLUME_MIN 0
#define VOLUME_MAX 100

static int s_volume = 70;

/* ------------------------------------------------------------------ art -- */
/* 16x16 one-bit bitmaps. Each byte is one row; bit 7 (MSB) is x=0 (left). */
static const uint8_t LOW_BATTERY_ART[32] = {
    0xFF, 0xFC, 0x80, 0x04, 0x80, 0x04, 0x80, 0x04,
    0x80, 0x04, 0x80, 0x04, 0x80, 0x04, 0x80, 0x04,
    0x80, 0x04, 0x80, 0x04, 0x80, 0x04, 0x83, 0xC4,
    0x83, 0xC4, 0x80, 0x04, 0x80, 0x04, 0xFF, 0xFC,
};

static const uint8_t ADMIN_ART[32] = {
    0x00, 0x00, 0x00, 0x00, 0x0F, 0xF0, 0x18, 0x18,
    0x30, 0x0C, 0x01, 0x80, 0x07, 0xE0, 0x0C, 0x30,
    0x01, 0x80, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* 3x5 digit font. Each digit is 3 columns; each byte is one column with bit 0
 * at the top row and bit 4 at the bottom row. */
static const uint8_t DIGIT_FONT[10][3] = {
    { 0x1F, 0x11, 0x1F }, /* 0 */
    { 0x00, 0x1F, 0x00 }, /* 1 */
    { 0x1D, 0x15, 0x17 }, /* 2 */
    { 0x15, 0x15, 0x1F }, /* 3 */
    { 0x07, 0x04, 0x1F }, /* 4 */
    { 0x17, 0x15, 0x1D }, /* 5 */
    { 0x1F, 0x15, 0x1D }, /* 6 */
    { 0x01, 0x01, 0x1F }, /* 7 */
    { 0x1F, 0x15, 0x1F }, /* 8 */
    { 0x17, 0x15, 0x1F }, /* 9 */
};

/* ------------------------------------------------------------ display --- */
/*
 * Draw a 16x16 one-bit bitmap into the HT16D35x framebuffer.
 *
 * @param[in] bmp 32-byte bitmap (1 byte per row, MSB = leftmost column).
 */
static void draw_bitmap(const uint8_t bmp[32])
{
    for (int y = 0; y < 16; y++)
    {
        uint8_t row = bmp[y];
        for (int x = 0; x < 16; x++)
        {
            ht16d35x_set_pixel(x, y, (row & 0x80) != 0);
            row <<= 1;
        }
    }
    ht16d35x_flush();
}

/*
 * Draw one 3x5 digit at column x (top-anchored at y=1).
 *
 * @param[in] x     Column of the digit's left edge (0..15).
 * @param[in] digit 0..9.
 */
static void draw_digit(int x, int digit)
{
    if (digit < 0 || digit > 9 || x < 0 || x + 3 > 16)
    {
        return;
    }
    for (int col = 0; col < 3; col++)
    {
        uint8_t bits = DIGIT_FONT[digit][col];
        for (int row = 0; row < 5; row++)
        {
            ht16d35x_set_pixel(x + col, 1 + row, (bits & 0x01) != 0);
            bits >>= 1;
        }
    }
}

/*
 * Render a 4-digit decimal code centered on the 16x16 panel.
 *
 * @param[in] code 0..9999.
 */
static void draw_code(uint16_t code)
{
    ht16d35x_clear();
    int digits[4] = {
        (code / 1000) % 10,
        (code / 100) % 10,
        (code / 10) % 10,
        code % 10,
    };
    for (int i = 0; i < 4; i++)
    {
        draw_digit(1 + i * 4, digits[i]);
    }
    ht16d35x_flush();
}

/*
 * Show the 4-digit admin code on the display (admin_code_cb_t callback).
 */
static void show_admin_code(uint16_t code)
{
    ESP_LOGI(TAG, "admin code: %04u", (unsigned int)code);
    draw_code(code);
}

/* ------------------------------------------------------------- encoder --- */
/*
 * Encoder callback: encoder 0 = volume, encoder 1 = track skip, button =
 * pause/resume.
 */
static void encoder_cb(int encoder_id, int delta, bool button)
{
    if (encoder_id == 0 && delta != 0)
    {
        s_volume += delta * VOLUME_DELTA_PER_DETENT;
        if (s_volume < VOLUME_MIN)
        {
            s_volume = VOLUME_MIN;
        }
        if (s_volume > VOLUME_MAX)
        {
            s_volume = VOLUME_MAX;
        }
        audio_set_volume(s_volume);
        ESP_LOGI(TAG, "volume %d", s_volume);
    }
    else if (encoder_id == 1 && delta != 0)
    {
        /* TODO: advance/rewind within a playlist; single-track mode is a no-op. */
        ESP_LOGI(TAG, "skip %d", delta);
    }

    if (button)
    {
        if (audio_is_playing())
        {
            audio_pause();
        }
        else
        {
            audio_resume();
        }
    }
}

/* ------------------------------------------------------------ app_main --- */
void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* I2C bus + IO expanders first. */
    ESP_ERROR_CHECK(iox_init());
    ESP_ERROR_CHECK(battery_init());
    ESP_ERROR_CHECK(ht16d35x_init());
    ESP_ERROR_CHECK(cr95hf_init());
    ESP_ERROR_CHECK(encoder_init());
    encoder_register_cb(encoder_cb);
    ESP_ERROR_CHECK(content_init());
    ESP_ERROR_CHECK(audio_init());
    audio_set_volume(s_volume);
    admin_set_code_callback(show_admin_code);

    ESP_LOGI(TAG, "boot complete (battery %d%%, %.1f mV)",
             battery_soc(), (double)battery_voltage());

    /* Boot-time low-battery check. */
    if (battery_is_low())
    {
        ESP_LOGW(TAG, "low battery");
        draw_bitmap(LOW_BATTERY_ART);
    }

    uint8_t uid[10];
    char url[128];

    while (1)
    {
        if (admin_is_active())
        {
            /* Web server runs in its own task; the code is already on screen. */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        uint8_t uid_len = sizeof(uid);
        if (cr95hf_poll(uid, &uid_len, url, sizeof(url)))
        {
            ESP_LOGI(TAG, "card: UID len=%u URL=%s", uid_len, url);

            if (strcmp(url, MAGIC_URL) == 0)
            {
                /* Toggle admin mode. */
                if (admin_is_active())
                {
                    admin_stop();
                    draw_bitmap(ADMIN_ART);
                }
                else
                {
                    uint16_t code = 0;
                    if (admin_start(&code) == ESP_OK)
                    {
                        ESP_LOGI(TAG, "admin mode on, code %04u", (unsigned int)code);
                        draw_code(code);
                    }
                    else
                    {
                        ESP_LOGE(TAG, "admin_start failed");
                    }
                }
            }
            else
            {
                char sound_path[128] = { 0 };
                char image_path[128] = { 0 };
                if (content_lookup(url, sound_path, sizeof(sound_path),
                                   image_path, sizeof(image_path)) == ESP_OK)
                {
                    ESP_LOGI(TAG, "playing %s (image %s)", sound_path, image_path);
                    /* TODO: decode + draw the 16x16 image from image_path. */
                    audio_play(sound_path);
                }
                else
                {
                    ESP_LOGW(TAG, "no content for URL %s", url);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
