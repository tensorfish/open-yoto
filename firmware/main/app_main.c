/*
 * app_main.c — Yoto replacement firmware: state machine.
 *
 * Boot initializes the player, mounts SD, starts the `openyoto` SoftAP and
 * authenticated web UI, then continues normal NFC/encoder playback. The
 * six-character admin code is rendered on the player display at startup.
 * A magic NDEF card can still explicitly toggle admin mode.
 */
#include <stdio.h>
#include <string.h>

#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "board_pins.h"
#include "iox.h"
#include "battery.h"
#include "lis2dh12.h"
#include "display.h"
#include "cr95hf.h"
#include "encoder.h"
#include "content.h"
#include "audio.h"
#include "admin.h"
#include "yoto_vfs.h"
#include "stock_low_battery_rgba.h"
#include "requested_factory_asset_rgba.h"
static const char *TAG = "main";

/* URL provisioned to an empty Type-2 card for the local admin endpoint. */
#define MAGIC_URL "https://openyoto.com/admin"

/* Number of encoder detents per volume step. */
#define VOLUME_DELTA_PER_DETENT 5
#define VOLUME_MIN 0
#define VOLUME_MAX 100

#ifdef CONFIG_APP_SPEAKER_TEST_TONE
static int s_volume = 100;
#else
static int s_volume = 70;
#endif

/* ------------------------------------------------------------------ art -- */
/* 16x16 one-bit bitmaps: 2 bytes per row (32 bytes total). Each byte holds 8
 * pixels; bit 7 (MSB) is the leftmost pixel of that byte. */

#define PLAYER_COLOR_IMAGE_MAGIC "OYIM"
#define PLAYER_COLOR_IMAGE_VERSION 1
#define PLAYER_COLOR_IMAGE_RGB565 1
#define PLAYER_COLOR_IMAGE_HEADER_SIZE 8
#define PLAYER_COLOR_IMAGE_16_WIDTH 16
#define PLAYER_COLOR_IMAGE_16_DATA_SIZE \
    (PLAYER_COLOR_IMAGE_16_WIDTH * PLAYER_COLOR_IMAGE_16_WIDTH * 2)
#define PLAYER_COLOR_IMAGE_16_FILE_SIZE \
    (PLAYER_COLOR_IMAGE_HEADER_SIZE + PLAYER_COLOR_IMAGE_16_DATA_SIZE)
#define PLAYER_COLOR_IMAGE_64_WIDTH 64
#define PLAYER_COLOR_IMAGE_64_DATA_SIZE \
    (PLAYER_COLOR_IMAGE_64_WIDTH * PLAYER_COLOR_IMAGE_64_WIDTH * 2)
#define PLAYER_COLOR_IMAGE_64_FILE_SIZE \
    (PLAYER_COLOR_IMAGE_HEADER_SIZE + PLAYER_COLOR_IMAGE_64_DATA_SIZE)


/* ------------------------------------------------------------ display --- */
/*
 * Draw a 16x16 one-bit bitmap into the display framebuffer.
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
            display_set_pixel(x, y, (row & 0x8000) != 0);
            row <<= 1;
        }
    }
    display_flush();
}

static void show_admin_code(
    const char code[ADMIN_ACCESS_CODE_LEN + 1])
{
    ESP_LOGI(TAG, "admin code: %s", code);
    display_show_access_code(code);
}

/* ------------------------------------------------------------- encoder --- */
/* Playback/power state, guarded by s_state_mutex (shared by the encoder task
 * and the main loop). */
static bool s_powered_off = false;
static char s_current_url[CR95HF_URL_MAX + 1];
static int s_track_index = 0;
static int s_track_count = 0;
static SemaphoreHandle_t s_state_mutex;

/* Serialize access to playback/track state across the two tasks. */
static void state_lock(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
}

static void state_unlock(void)
{
    xSemaphoreGive(s_state_mutex);
}

/* Toggle play/pause for the currently loaded audio. */
static void play_pause_toggle(void)
{
    state_lock();
    if (audio_is_paused())
    {
        audio_resume();
    }
    else if (audio_is_playing())
    {
        audio_pause();
    }
    state_unlock();
}

/* Toggle power: "off" stops audio, ends admin mode, and blanks the display. */
static void power_toggle(void)
{
    state_lock();
    s_powered_off = !s_powered_off;
    if (s_powered_off)
    {
        audio_stop();
        s_track_count = 0;
        s_track_index = 0;
        s_current_url[0] = '\0';
        if (admin_is_active())
        {
            admin_stop();
        }
        display_clear();
        display_flush();
        ESP_LOGI(TAG, "powered off");
    }
    else
    {
        ESP_LOGI(TAG, "powered on");
    }
    state_unlock();
}

static esp_err_t render_image(const char *path);

/* Advance/rewind the current card's tracks by delta (wraps around). */
static void skip_track(int delta)
{
    char sound_path[128];
    char image_path[128];

    state_lock();
    if (s_track_count > 1)
    {
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

        if (content_get_track_image(s_current_url, s_track_index,
                                    image_path, sizeof(image_path)) == ESP_OK)
        {
            render_image(image_path);
        }
        else
        {
            display_clear();
            display_flush();
        }
    }
    state_unlock();
}

/* Collapse rapid multi-button gestures into a single action so simultaneous
 * presses don't double-toggle power or cancel a play/pause. */
#define GESTURE_DEBOUNCE_MS      200
#define POWER_DEBOUNCE_MS        1000
static uint32_t s_last_playpause_ticks = 0;
static uint32_t s_last_power_ticks = 0;

/*
 * Return true when a gesture of a given kind was already handled within the
 * debounce window (and so should be suppressed); latch the timestamp otherwise.
 */
static bool gesture_debounced(uint32_t *last, uint32_t window_ms)
{
    uint32_t now = xTaskGetTickCount();

    if (*last != 0 && (int32_t)(now - *last) < pdMS_TO_TICKS(window_ms))
    {
        return true;
    }
    *last = now;
    return false;
}

/*
 * Encoder callback: encoder 0 = volume, encoder 1 = track skip; both knobs
 * short-press = play/pause; encoder 1 long-press and the power button = power.
 */
static void encoder_cb(int encoder_id, int delta, encoder_event_t event)
{
    bool power_gesture =
        (event == ENCODER_EVT_LONG_PRESS &&
         (encoder_id == ENCODER_ID_1 || encoder_id == ENCODER_ID_POWER)) ||
        (event == ENCODER_EVT_SHORT_PRESS && encoder_id == ENCODER_ID_POWER);

    /* While "off", ignore everything except the power-toggle gestures. */
    if (s_powered_off && !power_gesture)
    {
        return;
    }

    if (event == ENCODER_EVT_TURN)
    {
        if (encoder_id == ENCODER_ID_0)
        {
            s_volume -= delta * VOLUME_DELTA_PER_DETENT;
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
            if (!gesture_debounced(&s_last_playpause_ticks, GESTURE_DEBOUNCE_MS))
            {
                play_pause_toggle();
            }
        }
        else if (encoder_id == ENCODER_ID_POWER)
        {
            if (!gesture_debounced(&s_last_power_ticks, POWER_DEBOUNCE_MS))
            {
                power_toggle();
            }
        }
    }
    else if (event == ENCODER_EVT_LONG_PRESS)
    {
        if (encoder_id == ENCODER_ID_1 || encoder_id == ENCODER_ID_POWER)
        {
            if (!gesture_debounced(&s_last_power_ticks, POWER_DEBOUNCE_MS))
            {
                power_toggle();
            }
        }
    }
}


/* Battery is re-checked on this period in the main loop. */
#define BATTERY_CHECK_PERIOD_MS 30000
static uint32_t s_battery_check_ticks = 0;

/*
 * Render a legacy 16x16 one-bit image or an OYIM v1 RGB565 image. New browser
 * output is 16x16 color; previously generated 64x64 OYIM files remain valid.
 * Accepts an absolute /sdcard path or a content-relative card-mapping path.
 */
static esp_err_t render_image(const char *path)
{
    char full[160];
    FILE *f;
    long file_size;

    if (path == NULL || path[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(path, CONTENT_MOUNT_POINT "/",
                strlen(CONTENT_MOUNT_POINT) + 1) == 0)
    {
        snprintf(full, sizeof(full), "%s", path);
    }
    else
    {
        snprintf(full, sizeof(full), "%s/%s", CONTENT_MOUNT_POINT, path);
    }

    f = fopen(full, "rb");
    if (f == NULL)
    {
        ESP_LOGW(TAG, "cannot open image %s", full);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0
        || (file_size = ftell(f)) < 0
        || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        ESP_LOGW(TAG, "cannot inspect image %s", full);
        return ESP_FAIL;
    }

    if (file_size == 32)
    {
        uint8_t bmp[32];

        if (fread(bmp, 1, sizeof(bmp), f) != sizeof(bmp))
        {
            fclose(f);
            return ESP_FAIL;
        }
        fclose(f);
        draw_bitmap(bmp);
        return ESP_OK;
    }

    if (file_size == PLAYER_COLOR_IMAGE_16_FILE_SIZE
        || file_size == PLAYER_COLOR_IMAGE_64_FILE_SIZE)
    {
        uint8_t header[PLAYER_COLOR_IMAGE_HEADER_SIZE];
        uint8_t width;

        if (fread(header, 1, sizeof(header), f) != sizeof(header)
            || memcmp(header, PLAYER_COLOR_IMAGE_MAGIC, 4) != 0
            || header[4] != PLAYER_COLOR_IMAGE_VERSION
            || header[5] != PLAYER_COLOR_IMAGE_RGB565
            || header[6] != header[7])
        {
            fclose(f);
            ESP_LOGW(TAG, "invalid color image header in %s", full);
            return ESP_ERR_INVALID_RESPONSE;
        }
        width = header[6];

        if (width == PLAYER_COLOR_IMAGE_16_WIDTH
            && file_size == PLAYER_COLOR_IMAGE_16_FILE_SIZE)
        {
            uint8_t pixel_bytes[PLAYER_COLOR_IMAGE_16_DATA_SIZE];
            uint16_t pixels[PLAYER_COLOR_IMAGE_16_WIDTH
                            * PLAYER_COLOR_IMAGE_16_WIDTH];

            if (fread(pixel_bytes, 1, sizeof(pixel_bytes), f)
                != sizeof(pixel_bytes))
            {
                fclose(f);
                return ESP_FAIL;
            }
            for (size_t i = 0;
                 i < PLAYER_COLOR_IMAGE_16_WIDTH
                     * PLAYER_COLOR_IMAGE_16_WIDTH;
                 i++)
            {
                pixels[i] = (uint16_t)pixel_bytes[i * 2]
                          | (uint16_t)((uint16_t)pixel_bytes[i * 2 + 1] << 8);
            }
            fclose(f);
            return display_show_rgb56516(pixels);
        }

        if (width == PLAYER_COLOR_IMAGE_64_WIDTH
            && file_size == PLAYER_COLOR_IMAGE_64_FILE_SIZE)
        {
            uint8_t row_bytes[PLAYER_COLOR_IMAGE_64_WIDTH * 2];
            uint16_t row[PLAYER_COLOR_IMAGE_64_WIDTH];
            esp_err_t err = display_color64_begin();
            esp_err_t end_err;

            if (err != ESP_OK)
            {
                fclose(f);
                return err;
            }
            for (uint8_t y = 0; y < PLAYER_COLOR_IMAGE_64_WIDTH; y++)
            {
                if (fread(row_bytes, 1, sizeof(row_bytes), f)
                    != sizeof(row_bytes))
                {
                    ESP_LOGW(TAG, "short color image row %u in %s",
                             (unsigned)y, full);
                    err = ESP_FAIL;
                    break;
                }
                for (size_t x = 0; x < PLAYER_COLOR_IMAGE_64_WIDTH; x++)
                {
                    row[x] = (uint16_t)row_bytes[x * 2]
                           | (uint16_t)((uint16_t)row_bytes[x * 2 + 1] << 8);
                }
                err = display_color64_write_row(y, row);
                if (err != ESP_OK)
                {
                    break;
                }
            }
            end_err = display_color64_end();
            fclose(f);
            if (err == ESP_OK)
            {
                err = end_err;
            }
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "color image render failed for %s: %s",
                         full, esp_err_to_name(err));
            }
            return err;
        }

        fclose(f);
        ESP_LOGW(TAG, "unsupported OYIM dimensions %ux%u in %s",
                 (unsigned)header[6], (unsigned)header[7], full);
        return ESP_ERR_INVALID_SIZE;
    }

    fclose(f);
    ESP_LOGW(TAG, "image %s is %ld bytes; expected 32, %u, or %u",
             full, file_size, (unsigned)PLAYER_COLOR_IMAGE_16_FILE_SIZE,
             (unsigned)PLAYER_COLOR_IMAGE_64_FILE_SIZE);
    return ESP_ERR_INVALID_SIZE;
}

static esp_err_t remote_play_sound(const char *absolute_sd_path)
{
    state_lock();
    esp_err_t err = audio_play(absolute_sd_path);
    state_unlock();
    return err;
}

static esp_err_t remote_display_image(const char *absolute_sd_path)
{
    state_lock();
    esp_err_t err = render_image(absolute_sd_path);
    state_unlock();
    return err;
}

static esp_err_t remote_stop_sound(void)
{
    state_lock();
    esp_err_t err = audio_stop();
    state_unlock();
    return err;
}

static esp_err_t remote_clear_display(void)
{
    state_lock();
    display_clear();
    display_flush();
    state_unlock();
    return ESP_OK;
}

static esp_err_t remote_write_card(const char *url,
                                   const uint8_t *expected_uid,
                                   uint8_t uid_len)
{
    return cr95hf_write_url(url, expected_uid, uid_len);
}

/*
 * Draw a battery level bar (fill proportional to soc) and, when charging, a
 * lightning bolt above it. Used at boot and on each battery-check period.
 *
 * @param[in] soc       state of charge 0..100; -1 draws no fill.
 * @param[in] charging  true to draw the charging bolt.
 */
static void draw_battery_status(int soc, bool charging)
{
    static const uint8_t bolt[][2] = {
        { 8, 1 }, { 9, 1 }, { 7, 2 }, { 8, 2 }, { 6, 3 }, { 7, 3 }, { 8, 3 },
        { 7, 4 }, { 8, 4 }, { 9, 4 }, { 8, 5 }, { 9, 5 }, { 10, 5 },
        { 9, 6 }, { 10, 6 }, { 11, 6 }, { 10, 7 }, { 11, 7 }, { 10, 8 },
        { 11, 8 },
    };
    size_t i;

    display_clear();

    /* Horizontal bar: outline x 0..15, y 11..14; fill left-to-right. */
    for (int x = 0; x < 16; x++)
    {
        display_set_pixel(x, 11, true);
        display_set_pixel(x, 14, true);
    }
    for (int y = 11; y <= 14; y++)
    {
        display_set_pixel(0, y, true);
        display_set_pixel(15, y, true);
    }
    if (soc > 0)
    {
        int cols = (soc > 100 ? 100 : soc) * 14 / 100;
        for (int x = 1; x <= cols && x <= 14; x++)
        {
            for (int y = 12; y <= 13; y++)
            {
                display_set_pixel(x, y, true);
            }
        }
    }

    if (charging)
    {
        for (i = 0; i < sizeof(bolt) / sizeof(bolt[0]); i++)
        {
            display_set_pixel(bolt[i][0], bolt[i][1], true);
        }
    }

    display_flush();
}


/*
 * Check the battery and show the low-battery art when it is depleted.
 */
#ifdef CONFIG_APP_DISPLAY_TEST_ICON
static void show_display_test_image(void)
{
    display_show_rgba(REQUESTED_FACTORY_ASSET_RGBA);
}
#endif

static void battery_periodic_check(void)
{
    uint32_t now = xTaskGetTickCount();

    if ((int32_t)(now - s_battery_check_ticks) < pdMS_TO_TICKS(BATTERY_CHECK_PERIOD_MS))
    {
        return;
    }
    s_battery_check_ticks = now;

    if (battery_is_charging())
    {
        ESP_LOGI(TAG, "charging (SOC %d%%)", battery_soc());
        draw_battery_status(battery_soc(), true);
    }
    else if (battery_is_low())
    {
        ESP_LOGW(TAG, "low battery (%.1f mV, %d%%)",
                 (double)battery_voltage(), battery_soc());
#ifdef CONFIG_APP_DISPLAY_TEST_ICON
        show_display_test_image();
#else
        display_show_rgba(STOCK_LOW_BATTERY_RGBA);
#endif
    }
}

static const char *card_state_name(cr95hf_card_state_t state)
{
    switch (state)
    {
        case CR95HF_CARD_BLANK:
            return "blank";
        case CR95HF_CARD_URI:
            return "uri";
        case CR95HF_CARD_NON_URI:
            return "non-uri";
        case CR95HF_CARD_LOCKED:
            return "locked";
        case CR95HF_CARD_UNREADABLE:
        default:
            return "unreadable";
    }
}

/* Emit one bounded, human-readable card dump only on an insertion edge. */
static void log_card_diagnostics(const uint8_t *uid, uint8_t uid_len,
                                 const char *url,
                                 const cr95hf_card_info_t *info)
{
    char uid_hex[3 * CR95HF_UID_MAX + 1];
    size_t uid_off = 0;
    size_t off;

    for (size_t i = 0; i < uid_len && uid_off + 3 < sizeof(uid_hex); i++)
    {
        uid_off += (size_t)snprintf(&uid_hex[uid_off],
                                    sizeof(uid_hex) - uid_off, "%02x ", uid[i]);
    }
    uid_hex[uid_off] = '\0';
    ESP_LOGI(TAG,
             "card: uid=%s sak=%02x state=%s cc=%02x %02x %02x %02x locks=%02x %02x capacity=%u writable=%s url=%s",
             uid_hex, info->sak, card_state_name(info->state), info->cc[0],
             info->cc[1], info->cc[2], info->cc[3], info->lock0, info->lock1,
             (unsigned)info->capacity, info->writable ? "yes" : "no",
             url[0] == '\0' ? "<none>" : url);

    for (off = 0; off < info->raw_ndef_len; off += 16)
    {
        char bytes[3 * 16 + 1];
        size_t line_len = info->raw_ndef_len - off;
        size_t byte_off = 0;

        if (line_len > 16)
        {
            line_len = 16;
        }
        for (size_t i = 0; i < line_len; i++)
        {
            byte_off += (size_t)snprintf(&bytes[byte_off],
                                         sizeof(bytes) - byte_off, "%02x ",
                                         info->raw_ndef[off + i]);
        }
        bytes[byte_off] = '\0';
        ESP_LOGI(TAG, "card: pages %u-%u: %s",
                 (unsigned)(4 + off / 4),
                 (unsigned)(4 + (off + line_len - 1) / 4), bytes);
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

    ESP_ERROR_CHECK(yoto_vfs_init());

    /* Mutex serializing playback/track state between the encoder task and
     * this main loop. */
    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL)
    {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
        return;
    }

    /* I2C bus + IO expanders first. */
    ESP_ERROR_CHECK(iox_init());
    ESP_ERROR_CHECK(battery_init());
    if (lis2dh12_init() != ESP_OK)
    {
        ESP_LOGW(TAG, "accelerometer unavailable; continuing");
    }
    ESP_ERROR_CHECK(display_init());

    /* Boot-time battery check: render before the remaining peripheral init
     * so a later driver (UART/PCNT/SDMMC/I2S) can't disturb the panel. */
#ifdef CONFIG_APP_DISPLAY_TEST_ICON
    show_display_test_image();
#else
    if (battery_is_charging())
    {
        ESP_LOGI(TAG, "charging (SOC %d%%)", battery_soc());
        draw_battery_status(battery_soc(), true);
    }
    else if (battery_is_low())
    {
        ESP_LOGW(TAG, "low battery");
        display_show_rgba(STOCK_LOW_BATTERY_RGBA);
    }
#endif

    /* Audio is another recoverable peripheral. A missing amplifier/codec
     * must remain visible in logs without turning one hardware fault into a
     * watchdog-like reboot loop. Playback calls report invalid state later. */
    if (audio_init() != ESP_OK)
    {
        ESP_LOGW(TAG, "audio unavailable; continuing without playback");
    }
    audio_set_volume(s_volume);

#ifdef CONFIG_APP_SPEAKER_TEST_TONE
    if (audio_start_tone(1000) != ESP_OK)
    {
        ESP_LOGE(TAG, "speaker test tone failed to start");
    }
#endif
    /* NFC reader is optional at boot: a missing/unresponsive CR95HF (no
     * antenna, dead chip, bench rig) must not reboot-loop the device. */
    if (cr95hf_init() != ESP_OK)
    {
        ESP_LOGW(TAG, "NFC reader unavailable; continuing");
    }
    ESP_ERROR_CHECK(encoder_init());
    encoder_register_cb(encoder_cb);
    /* Stock user mode treats a successful FatFS mount as SD availability;
     * its optional higher-level indexes do not gate the mount. */
    esp_err_t content_err = content_init();
    if (content_err != ESP_OK)
    {
        ESP_LOGW(TAG, "SD/content initialization failed: %s",
                 esp_err_to_name(content_err));
    }
    if (content_err == ESP_OK)
    {
        esp_err_t startup_err = audio_play(YOTO_WELCOME_PATH);
        if (startup_err == ESP_OK)
        {
            ESP_LOGI(TAG, "stock welcome queued: %s", YOTO_WELCOME_PATH);
        }
        else
        {
            ESP_LOGW(TAG, "stock welcome unavailable at %s: %s",
                     YOTO_WELCOME_PATH, esp_err_to_name(startup_err));
        }
    }
    admin_set_code_callback(show_admin_code);
    admin_set_path_callbacks(remote_play_sound, remote_display_image,
                             remote_stop_sound, remote_clear_display);
    admin_set_card_write_callback(remote_write_card);
    {
        char code[ADMIN_ACCESS_CODE_LEN + 1];
        esp_err_t admin_err = admin_start(code, sizeof(code));

        if (admin_err == ESP_OK)
        {
            ESP_LOGI(TAG,
                     "admin active at boot (SSID=openyoto, code=%s, http://192.168.4.1/)",
                     code);
        }
        else
        {
            ESP_LOGE(TAG, "admin startup failed: %s",
                     esp_err_to_name(admin_err));
        }
    }

    ESP_LOGI(TAG, "boot complete (battery %d%%, %.1f mV)",
             battery_soc(), (double)battery_voltage());

    uint8_t uid[CR95HF_UID_MAX];
    char url[CR95HF_URL_MAX + 1];
    bool card_present = false;
    uint8_t last_uid[CR95HF_UID_MAX];
    uint8_t last_uid_len = 0;

    while (1)
    {
        if (s_powered_off)
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* Diagnostic provisioning mode: inspect each detected Type-2 card. */
        cr95hf_card_info_t card_info;
        uint8_t uid_len = sizeof(uid);
        bool card = cr95hf_poll_card(uid, &uid_len, url, sizeof(url),
                                     &card_info);
        /* A card is "new" on a rising edge, or when its UID changes while a
         * card is held (a swap within one poll window). */
        bool new_card = card && !card_present;
        if (card && card_present
            && (uid_len != last_uid_len
                || memcmp(uid, last_uid, uid_len) != 0))
        {
            new_card = true;
        }

        if (new_card)
        {
            esp_err_t write_err;

            memcpy(last_uid, uid, uid_len);
            last_uid_len = uid_len;
            log_card_diagnostics(uid, uid_len, url, &card_info);
            admin_set_last_card(uid, uid_len, url);

            state_lock();
            audio_stop();
            s_track_count = 0;
            s_track_index = 0;
            s_current_url[0] = '\0';
            state_unlock();
            if (card_info.state != CR95HF_CARD_BLANK)
            {
                ESP_LOGI(TAG, "card: not blank; left unchanged");
            }
            else
            {
                ESP_LOGI(TAG, "card: blank; provisioning %s", MAGIC_URL);
                write_err = cr95hf_write_url(MAGIC_URL, uid, uid_len);
                if (write_err == ESP_OK)
                {
                    admin_set_last_card(uid, uid_len, MAGIC_URL);
                    ESP_LOGI(TAG, "card: provisioned %s", MAGIC_URL);
                }
                else
                {
                    ESP_LOGE(TAG, "card: provisioning failed: %s",
                             esp_err_to_name(write_err));
                }
            }

        }
        else if (!card && card_present)
        {
            /* Card removed: stop playback so controls don't act on a ghost
             * card. */
            state_lock();
            if (s_track_count > 0)
            {
                audio_stop();
                s_track_count = 0;
                s_track_index = 0;
                s_current_url[0] = '\0';
            }
            state_unlock();
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
