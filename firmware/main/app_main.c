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
/* 16x16 one-bit bitmaps: 2 bytes per row (32 bytes total). Each byte holds 8
 * pixels; bit 7 (MSB) is the leftmost pixel of that byte. */
static const uint8_t LOW_BATTERY_ART[32] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x04, 0x60, 0x0C, 0x70,
    0x0C, 0x70, 0x0F, 0xF0, 0x0F, 0xF0, 0x0F, 0xF0,
    0x0F, 0xF0, 0x0F, 0xF0, 0x0F, 0xF0, 0x0F, 0xF0,
};

/* (admin-mode indicator removed — the 4-digit code is shown instead.) */

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
 * @param[in] bmp 32-byte bitmap (2 bytes per row; MSB of each byte is the
 *                leftmost pixel of that byte).
 */
static void draw_bitmap(const uint8_t bmp[32])
{
    for (int y = 0; y < 16; y++)
    {
        uint16_t row = ((uint16_t)bmp[y * 2] << 8) | bmp[y * 2 + 1];
        for (int x = 0; x < 16; x++)
        {
            ht16d35x_set_pixel(x, y, (row & 0x8000) != 0);
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
/* Playback/power state. */
static bool s_powered_off = false;
static char s_current_url[128];
static int s_track_index = 0;
static int s_track_count = 0;

/* Toggle play/pause for the currently loaded audio. */
static void play_pause_toggle(void)
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

/* Toggle power: "off" stops audio and blanks the display; "on" resumes. */
static void power_toggle(void)
{
    s_powered_off = !s_powered_off;
    if (s_powered_off)
    {
        audio_stop();
        ht16d35x_clear();
        ht16d35x_flush();
        ESP_LOGI(TAG, "powered off");
    }
    else
    {
        ESP_LOGI(TAG, "powered on");
    }
}

/* Advance/rewind the current card's tracks by delta (wraps around). */
static void skip_track(int delta)
{
    char sound_path[128];

    if (s_track_count <= 0)
    {
        return;
    }

    s_track_index += delta;
    s_track_index %= s_track_count;
    if (s_track_index < 0)
    {
        s_track_index += s_track_count;
    }

    if (content_get_track(s_current_url, s_track_index,
                          sound_path, sizeof(sound_path)) == ESP_OK)
    {
        ESP_LOGI(TAG, "track %d/%d: %s", s_track_index + 1,
                 s_track_count, sound_path);
        audio_play(sound_path);
    }
}

/*
 * Encoder callback: encoder 0 = volume, encoder 1 = track skip; both knobs
 * short-press = play/pause; encoder 1 long-press and the power button = power.
 */
static void encoder_cb(int encoder_id, int delta, encoder_event_t event)
{
    if (event == ENCODER_EVT_TURN)
    {
        if (encoder_id == ENCODER_ID_0)
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
        else if (encoder_id == ENCODER_ID_1)
        {
            skip_track(delta);
        }
    }
    else if (event == ENCODER_EVT_SHORT_PRESS)
    {
        if (encoder_id == ENCODER_ID_0 || encoder_id == ENCODER_ID_1)
        {
            play_pause_toggle();
        }
        else if (encoder_id == ENCODER_ID_POWER)
        {
            power_toggle();
        }
    }
    else if (event == ENCODER_EVT_LONG_PRESS)
    {
        if (encoder_id == ENCODER_ID_1 || encoder_id == ENCODER_ID_POWER)
        {
            power_toggle();
        }
    }
}


/* Battery is re-checked on this period in the main loop. */
#define BATTERY_CHECK_PERIOD_MS 30000
static uint32_t s_battery_check_ticks = 0;

/*
 * Render a 16x16 one-bit image file (32 bytes) to the display.
 *
 * @param[in] path path relative to the content mount point (e.g. "media/x.img").
 */
static void render_image(const char *path)
{
    char full[160];
    FILE *f;
    uint8_t bmp[32];
    size_t n;

    if (path == NULL || path[0] == '\0')
    {
        return;
    }

    snprintf(full, sizeof(full), "%s/%s", CONTENT_MOUNT_POINT, path);
    f = fopen(full, "rb");
    if (f == NULL)
    {
        ESP_LOGW(TAG, "cannot open image %s", full);
        return;
    }

    n = fread(bmp, 1, sizeof(bmp), f);
    fclose(f);

    if (n == sizeof(bmp))
    {
        draw_bitmap(bmp);
    }
}

/*
 * Check the battery and show the low-battery art when it is depleted.
 */
static void battery_periodic_check(void)
{
    uint32_t now = xTaskGetTickCount();

    if ((int32_t)(now - s_battery_check_ticks) < pdMS_TO_TICKS(BATTERY_CHECK_PERIOD_MS))
    {
        return;
    }
    s_battery_check_ticks = now;

    if (battery_is_low())
    {
        ESP_LOGW(TAG, "low battery (%.1f mV, %d%%)",
                 (double)battery_voltage(), battery_soc());
        draw_bitmap(LOW_BATTERY_ART);
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
    bool card_present = false;

    while (1)
    {
        if (s_powered_off)
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* Poll NFC in BOTH modes — the magic card toggles admin on and off. */
        uint8_t uid_len = sizeof(uid);
        bool card = cr95hf_poll(uid, &uid_len, url, sizeof(url));
        if (card && !card_present)
        {
            ESP_LOGI(TAG, "card: UID len=%u URL=%s", uid_len, url);

            if (strcmp(url, MAGIC_URL) == 0)
            {
                if (admin_is_active())
                {
                    admin_stop();
                    ht16d35x_clear();
                    ht16d35x_flush();
                    ESP_LOGI(TAG, "admin mode off");
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
            else if (!admin_is_active())
            {
                /* Content playback — normal mode only. */
                int n = content_get_track_count(url);
                if (n > 0)
                {
                    strncpy(s_current_url, url, sizeof(s_current_url) - 1);
                    s_current_url[sizeof(s_current_url) - 1] = '\0';
                    s_track_count = n;
                    s_track_index = 0;

                    char image_path[128] = { 0 };
                    content_lookup(url, NULL, 0, image_path, sizeof(image_path));

                    char sound_path[128] = { 0 };
                    if (content_get_track(url, 0, sound_path,
                                          sizeof(sound_path)) == ESP_OK)
                    {
                        ESP_LOGI(TAG, "playing track 1/%d: %s", n, sound_path);
                        render_image(image_path);
                        audio_play(sound_path);
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "no content for URL %s", url);
                }
            }
        }

        /* Remember presence so a held card acts once, not every poll. */
        card_present = card;

        /* Battery check only in normal mode (don't overwrite the admin code). */
        if (!admin_is_active())
        {
            battery_periodic_check();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
