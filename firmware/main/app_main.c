/*
 * app_main.c — Yoto replacement firmware: player state machine.
 *
 * Boot initializes the player and mounts SD. Normal NFC cards select mapped
 * playback; the exact admin magic card toggles the `openyoto` SoftAP and its
 * authenticated web UI. The six-character admin code is displayed only while
 * that mode is active.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
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
#include "battery_icons_rgba.h"
#include "boot_face_rgba.h"
#include "wink_face_rgba.h"
static const char *TAG = "main";

/* Exact NFC URI that toggles the on-demand admin endpoint. */
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

/* ------------------------------------------------------- boot recovery --- */
#define BOOT_RECOVERY_NAMESPACE "boot"
#define BOOT_RECOVERY_KEY "recovery"
#define BOOT_RECOVERY_MAGIC 0x4f595452u
#define BOOT_RECOVERY_VERSION 1u
#define BOOT_RECOVERY_MAX_RESTARTS 3u

typedef enum {
    BOOT_STAGE_IOX = 1,
    BOOT_STAGE_BATTERY,
    BOOT_STAGE_DISPLAY,
    BOOT_STAGE_AUDIO,
    BOOT_STAGE_NFC,
    BOOT_STAGE_ENCODER,
} boot_stage_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t restart_count;
    int32_t stage;
    int32_t error;
} boot_recovery_record_t;

static boot_recovery_record_t s_boot_recovery;
static const uint32_t s_boot_recovery_delays_ms[] = { 1000, 2000, 4000 };

static const char *boot_stage_name(boot_stage_t stage)
{
    switch (stage)
    {
        case BOOT_STAGE_IOX:
            return "IOX";
        case BOOT_STAGE_BATTERY:
            return "battery";
        case BOOT_STAGE_DISPLAY:
            return "display";
        case BOOT_STAGE_AUDIO:
            return "speaker/audio";
        case BOOT_STAGE_NFC:
            return "NFC";
        case BOOT_STAGE_ENCODER:
            return "encoder";
        default:
            return "unknown";
    }
}

static bool boot_recovery_record_valid(const boot_recovery_record_t *record)
{
    return record->magic == BOOT_RECOVERY_MAGIC
           && record->version == BOOT_RECOVERY_VERSION
           && record->restart_count > 0
           && record->restart_count <= BOOT_RECOVERY_MAX_RESTARTS
           && record->stage >= BOOT_STAGE_IOX
           && record->stage <= BOOT_STAGE_ENCODER;
}

static esp_err_t boot_recovery_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BOOT_RECOVERY_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_erase_key(handle, BOOT_RECOVERY_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        err = ESP_OK;
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void boot_recovery_prepare(void)
{
    nvs_handle_t handle;
    size_t record_size = sizeof(s_boot_recovery);
    esp_err_t err;

    memset(&s_boot_recovery, 0, sizeof(s_boot_recovery));
    if (esp_reset_reason() != ESP_RST_SW)
    {
        err = boot_recovery_clear();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "could not clear boot recovery record: %s",
                     esp_err_to_name(err));
        }
        return;
    }

    err = nvs_open(BOOT_RECOVERY_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "could not read boot recovery record: %s",
                 esp_err_to_name(err));
        return;
    }

    err = nvs_get_blob(handle, BOOT_RECOVERY_KEY, &s_boot_recovery,
                       &record_size);
    nvs_close(handle);
    if (err != ESP_OK || record_size != sizeof(s_boot_recovery)
        || !boot_recovery_record_valid(&s_boot_recovery))
    {
        memset(&s_boot_recovery, 0, sizeof(s_boot_recovery));
        err = boot_recovery_clear();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "could not discard boot recovery record: %s",
                     esp_err_to_name(err));
        }
    }
}

static void boot_recovery_hold(boot_stage_t stage, esp_err_t err)
{
    ESP_LOGE(TAG, "boot recovery exhausted at %s: %s; holding",
             boot_stage_name(stage), esp_err_to_name(err));
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void boot_recovery_restart(boot_stage_t stage, esp_err_t err)
{
    nvs_handle_t handle;
    esp_err_t nvs_err;

    if (s_boot_recovery.restart_count >= BOOT_RECOVERY_MAX_RESTARTS)
    {
        boot_recovery_hold(stage, err);
    }

    s_boot_recovery.magic = BOOT_RECOVERY_MAGIC;
    s_boot_recovery.version = BOOT_RECOVERY_VERSION;
    s_boot_recovery.restart_count++;
    s_boot_recovery.stage = stage;
    s_boot_recovery.error = err;

    nvs_err = nvs_open(BOOT_RECOVERY_NAMESPACE, NVS_READWRITE, &handle);
    if (nvs_err == ESP_OK)
    {
        nvs_err = nvs_set_blob(handle, BOOT_RECOVERY_KEY, &s_boot_recovery,
                               sizeof(s_boot_recovery));
        if (nvs_err == ESP_OK)
        {
            nvs_err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (nvs_err != ESP_OK)
    {
        ESP_LOGE(TAG, "cannot persist boot recovery record: %s",
                 esp_err_to_name(nvs_err));
        boot_recovery_hold(stage, err);
    }

    uint16_t attempt = s_boot_recovery.restart_count;
    ESP_LOGW(TAG, "boot recovery %u/%u at %s: %s; restarting in %u ms",
             (unsigned)attempt, (unsigned)BOOT_RECOVERY_MAX_RESTARTS,
             boot_stage_name(stage), esp_err_to_name(err),
             (unsigned)s_boot_recovery_delays_ms[attempt - 1]);
    vTaskDelay(pdMS_TO_TICKS(s_boot_recovery_delays_ms[attempt - 1]));
    esp_restart();
    boot_recovery_hold(stage, err);
}

static void boot_require(boot_stage_t stage, esp_err_t err)
{
    if (err != ESP_OK)
    {
        boot_recovery_restart(stage, err);
    }
}

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
    const char code[ADMIN_ACCESS_CODE_LEN + 1]);

/* ------------------------------------------------------------- encoder --- */
/* Playback/power state, guarded by s_state_mutex (shared by the encoder task
 * and the main loop). */
static bool s_powered_off = false;
static char s_current_url[CR95HF_URL_MAX + 1];
static int s_track_index = 0;
static int s_track_count = 0;
static SemaphoreHandle_t s_state_mutex;
typedef enum {
    DISPLAY_BASE_OFF,
    DISPLAY_BASE_IDLE,
    DISPLAY_BASE_CARD,
    DISPLAY_BASE_ADMIN,
    DISPLAY_BASE_BATTERY,
} display_base_t;

#define POWERED_BATTERY_VISUAL_MS 5000
/* The panel is refreshed once per coalesce window with the newest volume, never
 * once per detent. One window is one main-loop pass, so a fast turn still costs
 * at most one bar draw per pass. */
#define VOLUME_DRAW_COALESCE_MS      100
/* The overlay must outlive the coalesce window so the drawn bar is actually
 * visible. */
#define VOLUME_OVERLAY_MS           1500
#define IDLE_WINK_MS                300
#define BOOT_FACE_FPS                16

static display_base_t s_display_base = DISPLAY_BASE_IDLE;
static char s_display_image_path[160];
static char s_admin_code[ADMIN_ACCESS_CODE_LEN + 1];
static uint32_t s_battery_visual_deadline;
static uint32_t s_volume_overlay_deadline;
static uint32_t s_idle_wink_deadline;
static uint32_t s_volume_draw_due;
static bool s_volume_dirty;
static uint8_t s_wink_frame_index;

/* Serialize access to playback/track state across the two tasks. */
static void state_lock(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
}

static void state_unlock(void)
{
    xSemaphoreGive(s_state_mutex);
}
static esp_err_t render_image(const char *path);

static bool display_deadline_reached(uint32_t now, uint32_t deadline)
{
    return deadline != 0 && (int32_t)(now - deadline) >= 0;
}

static void display_set_idle_locked(void)
{
    s_display_base = DISPLAY_BASE_IDLE;
    s_display_image_path[0] = '\0';
    s_battery_visual_deadline = 0;
    s_idle_wink_deadline = 0;
    if (s_volume_overlay_deadline == 0)
    {
        display_show_rgba(IDLE_FACE_RGBA);
    }
}

/*
 * Pick the battery icon for a state of charge. Levels floor to the nearest
 * ten so the icon never overstates the charge, and anything under 10% (or an
 * unavailable reading) shows the empty icon.
 *
 * @param[in] soc state of charge 0..100; negative means unknown.
 */
static const uint8_t *battery_icon_for(int soc)
{
    int level = soc / 10;

    if (soc < 10)
    {
        return BATTERY_ICON_EMPTY;
    }
    if (level > 10)
    {
        level = 10;
    }
    /* Frames 1..10 are battery-10.png through battery-100.png. */
    return BATTERY_ICON_FRAMES[level];
}

static void display_render_battery_locked(void)
{
    const uint8_t *icon;

    if (battery_is_charging())
    {
        icon = BATTERY_ICON_CHARGING;
    }
    else if (battery_is_low())
    {
        /* battery_is_low() also fires on a sagging cell voltage at a healthy
         * state of charge, so the warning cannot be derived from the SOC icon
         * alone. battery-empty.png is the recovered stock low-battery slash. */
        icon = BATTERY_ICON_EMPTY;
    }
    else
    {
        icon = battery_icon_for(battery_soc());
    }
    display_show_rgba(icon);
    s_display_base = DISPLAY_BASE_BATTERY;
}

static void display_show_powered_battery_locked(void)
{
    display_render_battery_locked();
    /* Charging is an initial status, not a persistent base screen. A
     * sufficiently charged device always returns to its idle face. */
    if (!battery_is_low())
    {
        s_battery_visual_deadline =
            xTaskGetTickCount() + pdMS_TO_TICKS(POWERED_BATTERY_VISUAL_MS);
    }
}

static void display_play_boot_animation_locked(void)
{
    TickType_t start = xTaskGetTickCount();

    for (int i = 0; i < BOOT_FACE_FRAME_COUNT; i++)
    {
        if (i == 0)
        {
            /* Panel content is undefined after reset, so the first frame
             * blanks it; later frames rewrite every pixel of the icon window
             * and skip that 230 KB fill to hold the frame budget. */
            display_show_rgba(BOOT_FACE_FRAMES[i]);
        }
        else if (i < BOOT_FACE_FRAME_COUNT - 1)
        {
            display_show_rgba_frame(BOOT_FACE_FRAMES[i]);
        }
        else
        {
            /* The resting frame is face-08, which is also IDLE_FACE_RGBA.
             * Draw it through display_set_idle_locked() so the animation
             * ends on DISPLAY_BASE_IDLE with the display deadlines reset,
             * without drawing the final frame twice. */
            display_set_idle_locked();
        }

        TickType_t due =
            start + ((i + 1) * configTICK_RATE_HZ) / BOOT_FACE_FPS;
        int32_t remaining = (int32_t)(due - xTaskGetTickCount());
        if (remaining > 0)
        {
            vTaskDelay(remaining);
        }
    }
}

static void display_render_base_locked(void)
{
    switch (s_display_base)
    {
        case DISPLAY_BASE_OFF:
            display_clear();
            display_flush();
            break;
        case DISPLAY_BASE_IDLE:
            display_set_idle_locked();
            break;
        case DISPLAY_BASE_CARD:
            if (s_display_image_path[0] != '\0'
                && render_image(s_display_image_path) == ESP_OK)
            {
                break;
            }
            display_set_idle_locked();
            break;
        case DISPLAY_BASE_ADMIN:
            if (s_admin_code[0] != '\0')
            {
                display_show_access_code(s_admin_code);
                break;
            }
            display_set_idle_locked();
            break;
        case DISPLAY_BASE_BATTERY:
            display_render_battery_locked();
            break;
    }
}

static esp_err_t display_show_card_image_locked(const char *path)
{
    esp_err_t err = render_image(path);

    if (err == ESP_OK)
    {
        s_display_base = DISPLAY_BASE_CARD;
        snprintf(s_display_image_path, sizeof(s_display_image_path), "%s", path);
    }
    return err;
}

/*
 * Drop a pending coalesced volume draw along with the overlay itself. Every
 * screen that replaces the overlay must go through this: a draw that fires
 * after the overlay deadline was cleared would paint a bar that nothing
 * erases.
 */
static void display_cancel_volume_locked(void)
{
    s_volume_overlay_deadline = 0;
    s_volume_draw_due = 0;
    s_volume_dirty = false;
}

static void display_show_volume_locked(void)
{
    s_idle_wink_deadline = 0;
    /* Re-arming the draw deadline on every detent would starve the draw for
     * the whole time the user keeps turning; arm it only when no draw is
     * already pending. */
    if (!s_volume_dirty)
    {
        s_volume_dirty = true;
        s_volume_draw_due =
            xTaskGetTickCount() + pdMS_TO_TICKS(VOLUME_DRAW_COALESCE_MS);
    }
    s_volume_overlay_deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(VOLUME_OVERLAY_MS);
}

static void display_show_wink_locked(void)
{
    /* A right-knob twist is direct user feedback, so it outranks the battery
     * screen. Card art, the admin access code and an explicitly cleared screen
     * still win, as does an on-screen volume bar. */
    if ((s_display_base != DISPLAY_BASE_IDLE
         && s_display_base != DISPLAY_BASE_BATTERY)
        || s_volume_overlay_deadline != 0)
    {
        return;
    }
    /* Demote the battery screen instead of drawing over it, so the wink hold
     * expires back to the face rather than snapping to the battery icon. */
    s_display_base = DISPLAY_BASE_IDLE;
    s_display_image_path[0] = '\0';
    s_battery_visual_deadline = 0;
    display_show_rgba(WINK_FACE_FRAMES[s_wink_frame_index]);
    /* Alternate so consecutive right-knob turns cycle the two wink frames. */
    s_wink_frame_index = (uint8_t)(s_wink_frame_index ^ 1u);
    s_idle_wink_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(IDLE_WINK_MS);
}

static void display_maintain_locked(void)
{
    uint32_t now = xTaskGetTickCount();
    if (s_volume_dirty && display_deadline_reached(now, s_volume_draw_due))
    {
        s_volume_dirty = false;
        display_draw_volume_overlay(s_volume);
    }

    if (display_deadline_reached(now, s_volume_overlay_deadline))
    {
        display_cancel_volume_locked();
        display_render_base_locked();
    }
    if (s_volume_overlay_deadline != 0)
    {
        return;
    }
    if (display_deadline_reached(now, s_idle_wink_deadline))
    {
        s_idle_wink_deadline = 0;
        display_render_base_locked();
    }
    if (s_idle_wink_deadline != 0)
    {
        return;
    }
    if (display_deadline_reached(now, s_battery_visual_deadline)
        && s_display_base == DISPLAY_BASE_BATTERY)
    {
        s_battery_visual_deadline = 0;
        display_set_idle_locked();
    }
    /* The original provides named wink resources, but retained artifacts do
     * not prove an autonomous blink timer. Winks are event-driven only. */
}

static void show_admin_code(
    const char code[ADMIN_ACCESS_CODE_LEN + 1])
{
    state_lock();
    snprintf(s_admin_code, sizeof(s_admin_code), "%s", code);
    s_display_base = DISPLAY_BASE_ADMIN;
    s_battery_visual_deadline = 0;
    display_cancel_volume_locked();
    s_idle_wink_deadline = 0;
    display_show_access_code(s_admin_code);
    state_unlock();
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
        s_display_base = DISPLAY_BASE_OFF;
        s_battery_visual_deadline = 0;
        display_cancel_volume_locked();
        s_idle_wink_deadline = 0;
        display_render_base_locked();
        ESP_LOGI(TAG, "powered off");
    }
    else
    {
        display_show_powered_battery_locked();
        ESP_LOGI(TAG, "powered on");
    }
    state_unlock();
}


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
                                    image_path, sizeof(image_path)) != ESP_OK
            || display_show_card_image_locked(image_path) != ESP_OK)
        {
            display_set_idle_locked();
        }
    }
    else if (s_track_count == 0)
    {
        /* A right-knob turn with no card loaded is wink feedback. */
        display_show_wink_locked();
    }
    state_unlock();
}

/* Start the first track for a mapped NFC URI and establish skip-track state. */
static void play_card(const char *url)
{
    char sound_path[128];
    char image_path[128];
    int track_count = content_get_track_count(url);

    if (track_count <= 0)
    {
        ESP_LOGI(TAG, "card has no mapped tracks: %s",
                 url[0] == '\0' ? "<none>" : url);
        state_lock();
        display_set_idle_locked();
        state_unlock();
        return;
    }

    state_lock();
    s_track_index = 0;
    s_track_count = track_count;
    snprintf(s_current_url, sizeof(s_current_url), "%s", url);

    if (content_get_track(s_current_url, s_track_index,
                          sound_path, sizeof(sound_path)) != ESP_OK)
    {
        ESP_LOGW(TAG, "mapped card track became unavailable: %s", url);
        s_track_count = 0;
        s_current_url[0] = '\0';
        display_set_idle_locked();
        state_unlock();
        return;
    }

    ESP_LOGI(TAG, "track 1/%d: %s", s_track_count, sound_path);
    if (audio_play(sound_path) != ESP_OK)
    {
        ESP_LOGW(TAG, "could not play card track: %s", sound_path);
    }

    if (content_get_track_image(s_current_url, s_track_index,
                                image_path, sizeof(image_path)) != ESP_OK
        || display_show_card_image_locked(image_path) != ESP_OK)
    {
        display_set_idle_locked();
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
            state_lock();
            s_volume -= delta * VOLUME_DELTA_PER_DETENT;
            if (s_volume < VOLUME_MIN)
            {
                s_volume = VOLUME_MIN;
            }
            if (s_volume > VOLUME_MAX)
            {
                s_volume = VOLUME_MAX;
            }
            if (audio_set_volume(s_volume) == ESP_OK)
            {
                display_show_volume_locked();
                ESP_LOGI(TAG, "volume %d", s_volume);
            }
            else
            {
                ESP_LOGW(TAG, "could not set volume %d", s_volume);
            }
            state_unlock();
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
    esp_err_t err = display_show_card_image_locked(absolute_sd_path);
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
    s_display_base = DISPLAY_BASE_OFF;
    s_battery_visual_deadline = 0;
    display_cancel_volume_locked();
    s_idle_wink_deadline = 0;
    display_render_base_locked();
    state_unlock();
    return ESP_OK;
}

static esp_err_t remote_write_card(const char *url,
                                   const uint8_t *expected_uid,
                                   uint8_t uid_len)
{
    return cr95hf_write_url(url, expected_uid, uid_len);
}

/* Re-check the battery and refresh the battery icon while it is charging or
 * depleted. */
static void battery_periodic_check(void)
{
    uint32_t now = xTaskGetTickCount();

    if ((int32_t)(now - s_battery_check_ticks) < pdMS_TO_TICKS(BATTERY_CHECK_PERIOD_MS))
    {
        return;
    }
    s_battery_check_ticks = now;

    state_lock();
    if (battery_is_low())
    {
        /* A depleted battery is a warning, so it re-asserts itself over
         * whatever is on screen. */
        ESP_LOGW(TAG, "low battery (%.1f mV, %d%%)",
                 (double)battery_voltage(), battery_soc());
        s_battery_visual_deadline = 0;
        s_idle_wink_deadline = 0;
        if (s_volume_overlay_deadline == 0)
        {
            display_render_battery_locked();
        }
        else
        {
            s_display_base = DISPLAY_BASE_BATTERY;
        }
    }
    else if (battery_is_charging())
    {
        ESP_LOGI(TAG, "charging (SOC %d%%)", battery_soc());
        /* Charging is a status glimpse, not a base screen: refresh the icon
         * only while it already owns the display, so a wink, card art or the
         * idle face keeps it. */
        if (s_display_base == DISPLAY_BASE_BATTERY
            && s_volume_overlay_deadline == 0
            && s_idle_wink_deadline == 0)
        {
            display_render_battery_locked();
        }
    }
    else if (s_display_base == DISPLAY_BASE_BATTERY
             && s_battery_visual_deadline == 0)
    {
        display_set_idle_locked();
    }
    state_unlock();
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
    boot_recovery_prepare();


    ESP_ERROR_CHECK(yoto_vfs_init());

    /* Mutex serializing playback/track state between the encoder task and
     * this main loop. */
    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL)
    {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
        return;
    }

    /* IOX powers and connects the shared I2C peripherals, so it must be
     * healthy before their strict boot checks. */
    boot_require(BOOT_STAGE_IOX, iox_init());
    boot_require(BOOT_STAGE_BATTERY, battery_init());
    if (lis2dh12_init() != ESP_OK)
    {
        ESP_LOGW(TAG, "accelerometer unavailable; continuing");
    }
    boot_require(BOOT_STAGE_DISPLAY, display_init());

    /* Play the boot face animation and settle on the idle face. */
    state_lock();
    display_play_boot_animation_locked();
    /* Show the battery icon straight after the animation when it carries news:
     * a charging status clears itself after POWERED_BATTERY_VISUAL_MS, while a
     * depleted battery stays up. The periodic check only runs 30 s later. */
    if (battery_is_low() || battery_is_charging())
    {
        display_show_powered_battery_locked();
    }
    state_unlock();

    /* Speaker, codec, I2S, and NFC are required for normal player operation.
     * A failed boot is retried through the bounded full-system recovery gate. */
    boot_require(BOOT_STAGE_AUDIO, audio_init());
    audio_set_volume(s_volume);

#ifdef CONFIG_APP_SPEAKER_TEST_TONE
    if (audio_start_tone(1000) != ESP_OK)
    {
        ESP_LOGE(TAG, "speaker test tone failed to start");
    }
#endif
    boot_require(BOOT_STAGE_NFC, cr95hf_init());
    boot_require(BOOT_STAGE_ENCODER, encoder_init());
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
    /* These registrations remain installed across admin start/stop cycles. */
    admin_set_code_callback(show_admin_code);
    admin_set_path_callbacks(remote_play_sound, remote_display_image,
                             remote_stop_sound, remote_clear_display);
    admin_set_card_write_callback(remote_write_card);

    err = boot_recovery_clear();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "could not clear boot recovery record: %s",
                 esp_err_to_name(err));
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

        /* A new NFC card either toggles admin, is captured by active admin,
         * or starts its mapped first track. */
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
            memcpy(last_uid, uid, uid_len);
            last_uid_len = uid_len;
            log_card_diagnostics(uid, uid_len, url, &card_info);

            /* Replacing a card always stops the previous card before handling
             * the new card's mode or playback action. */
            state_lock();
            audio_stop();
            s_track_count = 0;
            s_track_index = 0;
            s_current_url[0] = '\0';
            state_unlock();

            if (strcmp(url, MAGIC_URL) == 0)
            {
                if (admin_is_active())
                {
                    esp_err_t err = admin_stop();
                    if (err != ESP_OK)
                    {
                        ESP_LOGE(TAG, "admin stop failed: %s",
                                 esp_err_to_name(err));
                    }
                    else
                    {
                        state_lock();
                        s_admin_code[0] = '\0';
                        display_set_idle_locked();
                        state_unlock();
                        ESP_LOGI(TAG, "admin inactive");
                    }
                }
                else
                {
                    char code[ADMIN_ACCESS_CODE_LEN + 1];
                    esp_err_t err = admin_start(code, sizeof(code));

                    if (err == ESP_OK)
                    {
                        ESP_LOGI(TAG,
                                 "admin active (SSID=openyoto, code=%s, http://192.168.4.1/)",
                                 code);
                    }
                    else
                    {
                        ESP_LOGE(TAG, "admin startup failed: %s",
                                 esp_err_to_name(err));
                    }
                }
            }
            else if (admin_is_active())
            {
                admin_set_last_card(uid, uid_len, url);
                ESP_LOGI(TAG, "admin captured card without playback");
            }
            else
            {
                play_card(url);
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
            if (s_display_base != DISPLAY_BASE_ADMIN)
            {
                display_set_idle_locked();
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

        state_lock();
        display_maintain_locked();
        state_unlock();

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
