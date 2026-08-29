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
#include "esp_sleep.h"
#include "esp_random.h"
#include "esp_attr.h"
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
 * and the main loop). Set before rail shutdown so concurrent tasks stop
 * producing work while the ESP32 is entering deep sleep. */
static bool s_powered_off = false;
/* Thirty minutes matches the stock player's inactivity shutdown policy. */
#define IDLE_POWER_OFF_MS (30u * 60u * 1000u)
static TickType_t s_last_activity_ticks;
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

/* A battery glimpse is transient: it covers the base screen for this long and
 * then hands the panel back. */
#define BATTERY_GLIMPSE_MS        5000
/* The panel is refreshed once per coalesce window with the newest volume, never
 * once per detent. One window is one main-loop pass, so a fast turn still costs
 * at most one bar draw per pass. */
#define VOLUME_DRAW_COALESCE_MS      100
/* The overlay must outlive the coalesce window so the drawn bar is actually
 * visible. */
#define VOLUME_OVERLAY_MS           1500
#define IDLE_WINK_MS                300
#define BOOT_FACE_FPS                16
/* With nothing playing, a volume change makes no sound at all, so a short blip
 * at the new level is the only feedback the user can hear. One blip per detent
 * gives continuous feedback while the knob turns; a dedicated task plays them so
 * the encoder never waits on I2S. */
#define VOLUME_BLIP_HZ              880
#define VOLUME_BLIP_MS               45
#define VOLUME_BLIP_STACK_BYTES    2560
#define VOLUME_BLIP_PRIORITY          3

static display_base_t s_display_base = DISPLAY_BASE_IDLE;
static char s_display_image_path[160];
static char s_admin_code[ADMIN_ACCESS_CODE_LEN + 1];
static uint32_t s_battery_visual_deadline;
static uint32_t s_volume_overlay_deadline;
static uint32_t s_idle_wink_deadline;
static uint32_t s_volume_draw_due;
/* Signalled once per volume detent; the feedback task plays one blip per take,
 * and extra gives collapse into one pending blip. */
static SemaphoreHandle_t s_volume_blip_signal;
static bool s_volume_dirty;
static uint8_t s_wink_frame_index;
/* Gestures the encoder task recorded for the gesture task to perform. */
static int s_pending_skip;
static bool s_pending_power;
static bool s_pending_screen_toggle;
/* The card a pending skip was captured for; a swap invalidates the turn. */
static char s_pending_skip_url[CR95HF_URL_MAX + 1];
/* Wakes the gesture task the moment a gesture is recorded. */
static SemaphoreHandle_t s_gesture_signal;
#define GESTURE_TASK_STACK_BYTES 6144
#define GESTURE_TASK_PRIORITY       4
/* Last sampled charger state; the not-charging -> charging edge is news. */
static bool s_charging_latched;

/* Serialize access to playback/track state across the tasks. */
static void state_lock(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
}

static void state_unlock(void)
{
    xSemaphoreGive(s_state_mutex);
}

/* Wake the gesture task; safe to call with the state mutex held. */
static void gesture_signal(void)
{
    if (s_gesture_signal != NULL)
    {
        xSemaphoreGive(s_gesture_signal);
    }
}
static esp_err_t render_image(const char *path);

static bool display_deadline_reached(uint32_t now, uint32_t deadline)
{
    return deadline != 0 && (int32_t)(now - deadline) >= 0;
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

/*
 * Re-apply the volume bar on top of a frame that was just painted. Every icon
 * frame rewrites the bar's rows, so a wink, a face or a battery icon erases the
 * bar; without this the two knobs fight over the panel instead of composing.
 */
static void display_apply_volume_overlay_locked(void)
{
    if (s_volume_overlay_deadline == 0)
    {
        return;
    }
    s_volume_dirty = false;
    display_draw_volume_overlay(s_volume);
}

/*
 * Make the idle face the base screen without painting it. Callers that paint a
 * transient straight afterwards use this; everything else uses
 * display_set_idle_locked(), which also puts the face on the panel.
 */
static void display_set_idle_base_locked(void)
{
    s_display_base = DISPLAY_BASE_IDLE;
    s_display_image_path[0] = '\0';
    s_battery_visual_deadline = 0;
    s_idle_wink_deadline = 0;
}

static void display_set_idle_locked(void)
{
    /* Admin mode owns the panel: the six-character code is the only way into
     * the web UI, so every "back to the face" path — card removal, an expiring
     * transient, a battery recovery — must land on the code, not the face. */
    if (admin_is_active() && s_admin_code[0] != '\0')
    {
        s_display_base = DISPLAY_BASE_ADMIN;
        s_battery_visual_deadline = 0;
        s_idle_wink_deadline = 0;
        display_show_access_code(s_admin_code);
        return;
    }
    display_set_idle_base_locked();
    display_show_rgba(IDLE_FACE_RGBA);
    display_apply_volume_overlay_locked();
}

/*
 * Pick the battery icon for a state of charge. Each icon covers the ten points
 * up to its label: battery-10.png is 0..10%, battery-20.png is 11..20%, and so
 * on to battery-100.png for 91..100%. Only an unavailable reading falls back to
 * the empty icon.
 *
 * @param[in] soc state of charge 0..100; negative means unknown.
 */
static const uint8_t *battery_icon_for(int soc)
{
    int level;

    if (soc < 0)
    {
        return BATTERY_ICON_EMPTY;
    }
    if (soc > 100)
    {
        soc = 100;
    }
    level = (soc + 9) / 10;
    if (level < 1)
    {
        level = 1;
    }
    /* Frames 1..10 are battery-10.png through battery-100.png. */
    return BATTERY_ICON_FRAMES[level];
}

/*
 * Pick the charging icon for a state of charge. Each frame covers the ten
 * points up to its label: battery-charging-0.png is 0..9%, battery-charging-10.png
 * is 10..19%, and so on to battery-charging-100.png at 100%. An unavailable
 * reading falls back to the 0% frame.
 *
 * @param[in] soc state of charge 0..100; negative means unknown.
 */
static const uint8_t *battery_charging_icon_for(int soc)
{
    int level;

    if (soc < 0)
    {
        soc = 0;
    }
    if (soc > 100)
    {
        soc = 100;
    }
    level = soc / 10;
    /* Frames 11..21 are battery-charging-0.png through battery-charging-100.png. */
    return BATTERY_ICON_FRAMES[11 + level];
}

/* The icon the battery currently warrants: the charge-level charging icon while
 * charging, otherwise the icon covering the state of charge. */
static const uint8_t *battery_icon_now(void)
{
    if (battery_is_charging())
    {
        return battery_charging_icon_for(battery_soc());
    }
    return battery_icon_for(battery_soc());
}

static void display_render_battery_locked(void)
{
    display_show_rgba(battery_icon_now());
    display_apply_volume_overlay_locked();
    s_display_base = DISPLAY_BASE_BATTERY;
}

/*
 * Show the battery icon as a transient glimpse: the base screen underneath is
 * left intact, and display_maintain_locked() restores it when the deadline
 * expires. A depleted battery is not a glimpse — it becomes the base screen and
 * stays until the battery recovers.
 */
static void display_show_battery_glimpse_locked(void)
{
    if (battery_is_low())
    {
        display_render_battery_locked();
        return;
    }
    /* The glimpse replaces a wink rather than racing its deadline, but a volume
     * bar still composes on top. */
    s_idle_wink_deadline = 0;
    display_show_rgba(battery_icon_now());
    display_apply_volume_overlay_locked();
    s_battery_visual_deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(BATTERY_GLIMPSE_MS);
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
            display_show_rgba(BOOT_FACE_FRAMES[i]);
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
                display_apply_volume_overlay_locked();
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

static void display_show_volume_locked(void)
{
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
     * still win. A live volume bar does not block the wink: the two compose,
     * with the bar re-applied over each wink frame. */
    if (s_display_base != DISPLAY_BASE_IDLE
        && s_display_base != DISPLAY_BASE_BATTERY)
    {
        ESP_LOGI(TAG, "wink suppressed (base %d)", (int)s_display_base);
        return;
    }
    /* Demote the battery screen instead of drawing over it, so the wink hold
     * expires back to the face rather than snapping to the battery icon. */
    display_set_idle_base_locked();
    display_show_rgba(WINK_FACE_FRAMES[s_wink_frame_index]);
    display_apply_volume_overlay_locked();
    ESP_LOGI(TAG, "wink frame %u", (unsigned)s_wink_frame_index);
    /* Alternate so consecutive right-knob turns cycle the two wink frames. */
    s_wink_frame_index = (uint8_t)(s_wink_frame_index ^ 1u);
    s_idle_wink_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(IDLE_WINK_MS);
}

static void display_maintain_locked(void)
{
    uint32_t now = xTaskGetTickCount();

    /*
     * Three transients can be live at once (wink, battery glimpse, volume bar).
     * Each expiry repaints the frame it covered, and the leaf painters re-apply
     * a still-live volume bar, so the panel is never left half-composed.
     */
    if (display_deadline_reached(now, s_idle_wink_deadline))
    {
        s_idle_wink_deadline = 0;
        display_render_base_locked();
    }
    if (display_deadline_reached(now, s_battery_visual_deadline))
    {
        s_battery_visual_deadline = 0;
        display_render_base_locked();
    }
    if (display_deadline_reached(now, s_volume_overlay_deadline))
    {
        /* Only the bar goes away here, so repaint what it covered and stop. */
        display_cancel_volume_locked();
        display_render_base_locked();
        return;
    }
    if (s_volume_dirty && display_deadline_reached(now, s_volume_draw_due))
    {
        s_volume_dirty = false;
        display_draw_volume_overlay(s_volume);
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

/*
 * The stock firmware does not implement "off" as a polling loop. It stops the
 * player, disconnects downstream rails, unlatches VIN_HOLD, then enters deep
 * sleep for the externally-powered case where the ESP32 remains supplied.
 * On battery, dropping VIN_HOLD removes power completely; the physical power
 * button subsequently starts a cold boot.
 */
static void power_off(bool wait_for_button_release)
{
    esp_err_t err;

    state_lock();
    if (s_powered_off)
    {
        state_unlock();
        return;
    }
    s_powered_off = true;
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
    state_unlock();

    if (wait_for_button_release)
    {
        /* The expander IRQ is level-low until its input port is read. Waiting
         * for release prevents an externally-powered unit from immediately
         * waking from the same long press that requested shutdown. */
        while (!iox_get_pin(IOX_BTN_POWER))
        {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    (void)iox_set_pin(IOX_AUDIO_PACTRL, false);
    err = iox_set_peripherals_powered(false);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "could not disconnect peripheral rails: %s",
                 esp_err_to_name(err));
    }

    /* Reading every input port clears pending PI4IOE5V6416 interrupts before
     * GPIO34 is armed as the active-low wake source. */
    uint8_t ignored;
    (void)iox_read_port(0, 0, &ignored);
    (void)iox_read_port(0, 1, &ignored);
#ifndef CONFIG_BOARD_REV_04
    (void)iox_read_port(1, 0, &ignored);
    (void)iox_read_port(1, 1, &ignored);
#endif
    err = esp_sleep_enable_ext0_wakeup(PIN_IOX_INT, 0);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "could not arm power-button wake: %s",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "powering off: rails disconnected, VIN_HOLD released");
    err = iox_set_pin(IOX_POWER_VINHOLD, false);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "could not release VIN_HOLD: %s",
                 esp_err_to_name(err));
    }
    esp_deep_sleep_start();
}

static void activity_note(void)
{
    s_last_activity_ticks = xTaskGetTickCount();
}

static void idle_power_check(void)
{
    TickType_t now = xTaskGetTickCount();

    if (s_powered_off)
    {
        return;
    }
    if (audio_is_playing() || admin_is_active())
    {
        s_last_activity_ticks = now;
        return;
    }
    if ((TickType_t)(now - s_last_activity_ticks)
            < pdMS_TO_TICKS(IDLE_POWER_OFF_MS))
    {
        return;
    }

    ESP_LOGI(TAG, "30 minutes inactive; powering off");
    power_off(false);
}

/* Short power presses blank or restore only the panel; rails stay powered. */
static void screen_toggle(void)
{
    state_lock();
    if (s_display_base == DISPLAY_BASE_OFF)
    {
        display_set_idle_base_locked();
        display_show_battery_glimpse_locked();
        ESP_LOGI(TAG, "screen on; showing battery status");
    }
    else
    {
        s_display_base = DISPLAY_BASE_OFF;
        s_battery_visual_deadline = 0;
        display_cancel_volume_locked();
        s_idle_wink_deadline = 0;
        display_render_base_locked();
        ESP_LOGI(TAG, "screen off");
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

    s_track_index = track_count > 1 ? (int)(esp_random() % (uint32_t)track_count)
                                    : 0;
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

    ESP_LOGI(TAG, "track %d/%d: %s", s_track_index + 1, s_track_count,
             sound_path);
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
 * Encoder callback: knob short presses toggle play/pause. Only a three-second
 * hold of the dedicated power button changes the board power state.
 */
static void encoder_cb(int encoder_id, int delta, encoder_event_t event)
{
    if (s_powered_off)
    {
        return;
    }
    activity_note();

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
                if (s_volume_blip_signal != NULL)
                {
                    xSemaphoreGive(s_volume_blip_signal);
                }
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
            state_lock();
            if (s_track_count > 1)
            {
                /* Skipping a track reads mapping.json off the SD card and
                 * renders card art, which needs far more stack than the 4 KB
                 * encoder task has — doing it here overflows and panics. Record
                 * the intent, tagged with the card it belongs to, and let the
                 * gesture task perform it. */
                s_pending_skip += delta;
                snprintf(s_pending_skip_url, sizeof(s_pending_skip_url), "%s",
                         s_current_url);
                gesture_signal();
            }
            else if (s_track_count == 0)
            {
                /* The wink is one constant frame straight to the panel, cheap
                 * enough to answer the knob immediately. */
                display_show_wink_locked();
            }
            state_unlock();
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
            state_lock();
            s_pending_screen_toggle = true;
            gesture_signal();
            state_unlock();
        }
    }
    else if (event == ENCODER_EVT_LONG_PRESS &&
             encoder_id == ENCODER_ID_POWER)
    {
        if (!gesture_debounced(&s_last_power_ticks, POWER_DEBOUNCE_MS))
        {
            /* The power operation tears down Wi-Fi and cycles board rails, so
             * run it outside the encoder task's small stack. */
            state_lock();
            s_pending_power = true;
            gesture_signal();
            state_unlock();
        }
    }
}

/*
 * Perform the gestures the encoder task deferred. The encoder callback runs on a
 * 4 KB task, so anything touching the SD card, the admin server or the image
 * renderer is recorded as intent and executed here, off that stack.
 *
 * Called from the gesture task (immediately, so a knob turn or power tap is not
 * left waiting behind the main loop's ~400 ms NFC poll) and once per main-loop
 * pass as a safety net if that task could not be created. Draining under the
 * lock makes the two callers idempotent.
 */
static void encoder_actions_pump(void)
{
    int skip;
    bool power;
    bool screen_toggle_pending;
    bool stale;

    state_lock();
    skip = s_pending_skip;
    s_pending_skip = 0;
    power = s_pending_power;
    s_pending_power = false;
    screen_toggle_pending = s_pending_screen_toggle;
    s_pending_screen_toggle = false;
    /* A skip belongs to the card that was loaded when the knob turned. The card
     * can be removed or swapped in between, and applying the turn to whatever
     * card arrived next would skip the wrong story. */
    stale = skip != 0 && strcmp(s_pending_skip_url, s_current_url) != 0;
    s_pending_skip_url[0] = '\0';
    state_unlock();

    if (power)
    {
        power_off(true);
    }
    if (screen_toggle_pending)
    {
        screen_toggle();
    }
    if (skip != 0 && !stale)
    {
        skip_track(skip);
    }
}

/*
 * Deferred gestures run here rather than on the main loop, which can be inside a
 * blocking NFC poll when the knob turns.
 */
static void gesture_task(void *arg)
{
    (void)arg;

    while (true)
    {
        if (xSemaphoreTake(s_gesture_signal, portMAX_DELAY) == pdTRUE)
        {
            encoder_actions_pump();
        }
    }
}

/*
 * Watch this task's stack margin. The card path (NFC poll -> mapping.json parse
 * -> image decode -> panel write) is the deepest chain the loop takes, and a
 * silent overflow there ends in a panic rather than an error, so complain while
 * there is still room. uxTaskGetStackHighWaterMark() reports StackType_t units,
 * which are bytes on the ESP32.
 */
#define STACK_WATCH_PERIOD_MS 10000
#define STACK_WATCH_MIN_BYTES  1024
static uint32_t s_stack_watch_ticks;

static void stack_watch(void)
{
    uint32_t now = xTaskGetTickCount();
    UBaseType_t headroom;

    if (s_stack_watch_ticks != 0
        && (int32_t)(now - s_stack_watch_ticks) < pdMS_TO_TICKS(STACK_WATCH_PERIOD_MS))
    {
        return;
    }
    s_stack_watch_ticks = now;
    headroom = uxTaskGetStackHighWaterMark(NULL);
    if (headroom < STACK_WATCH_MIN_BYTES)
    {
        ESP_LOGW(TAG, "main task stack headroom down to %u bytes",
                 (unsigned)headroom);
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
            /* Static, not stack: 1 KB of card-image buffers here plus the 1 KB
             * inside display_show_rgb56516() overflowed the main task. The
             * display path is serialised by the state mutex. */
            static uint8_t pixel_bytes[PLAYER_COLOR_IMAGE_16_DATA_SIZE];
            static uint16_t pixels[PLAYER_COLOR_IMAGE_16_WIDTH
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
static esp_err_t remote_pause_sound(void)
{
    state_lock();
    esp_err_t err = audio_pause();
    state_unlock();
    return err;
}

static esp_err_t remote_resume_sound(void)
{
    state_lock();
    esp_err_t err = audio_resume();
    state_unlock();
    return err;
}


static esp_err_t remote_clear_display(void)
{
    state_lock();
    if (admin_is_active() && s_admin_code[0] != '\0')
    {
        /* Clear returns the panel to the admin access code after a remote
         * preview, so the PIN is never lost while admin mode is active. */
        s_display_base = DISPLAY_BASE_ADMIN;
        display_show_access_code(s_admin_code);
        state_unlock();
        return ESP_OK;
    }
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
        /* Charging is announced by the charge-state edge, not by this period:
         * re-asserting it here would steal the face or card art back. */
        ESP_LOGI(TAG, "charging (SOC %d%%)", battery_soc());
    }
    else if (s_display_base == DISPLAY_BASE_BATTERY
             && s_battery_visual_deadline == 0)
    {
        display_set_idle_locked();
    }
    state_unlock();
}


/*
 * Volume feedback task: one blip per detent, so turning the knob gives
 * continuous sound instead of a single tone once it stops. It runs outside the
 * state mutex because audio_play_blip() blocks for the length of the blip, and
 * the binary semaphore collapses a burst of detents into at most one pending
 * blip so the queue can never run away from the knob.
 */
static void volume_blip_task(void *arg)
{
    (void)arg;

    while (true)
    {
        int volume;
        bool powered_off;
        esp_err_t err;

        if (xSemaphoreTake(s_volume_blip_signal, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        state_lock();
        volume = s_volume;
        powered_off = s_powered_off;
        state_unlock();

        /* Silence needs no blip, and content is its own volume feedback. */
        if (powered_off || volume <= VOLUME_MIN || audio_is_playing())
        {
            continue;
        }
        err = audio_play_blip(VOLUME_BLIP_HZ, VOLUME_BLIP_MS);
        /* A stream that started between the checks refuses the blip; that is
         * the intended precedence, not a fault. */
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGW(TAG, "volume blip failed: %s", esp_err_to_name(err));
        }
    }
}

/*
 * Charge state is edge-polled on this period so a plug-in shows its glimpse
 * promptly instead of waiting for the 30 s battery period.
 *
 * The charger's STAT line is open-drain and this unit's SGM41513 never ACKs on
 * I2C, so the level is all we have — and a charger that blinks STAT (fault or
 * end-of-charge indication) would otherwise re-arm the glimpse forever and keep
 * the face off the panel. Two defences: a transition counts only after the new
 * level holds for CHARGE_EDGE_STABLE_POLLS consecutive samples, and a glimpse
 * is never shown more often than CHARGE_GLIMPSE_MIN_GAP_MS.
 */
#define CHARGE_EDGE_POLL_MS          500
#define CHARGE_EDGE_STABLE_POLLS       4
#define CHARGE_GLIMPSE_MIN_GAP_MS  30000
static uint32_t s_charge_poll_ticks;
static uint32_t s_charge_glimpse_ticks;
static bool s_charge_sample;
static uint8_t s_charge_agree;

/*
 * Announce a debounced not-charging -> charging edge with a battery glimpse.
 * Unplugging needs no announcement: the glimpse expires back to the base screen
 * on its own.
 */
static void charge_edge_check(void)
{
    uint32_t now = xTaskGetTickCount();
    bool charging;

    if (s_charge_poll_ticks != 0
        && (int32_t)(now - s_charge_poll_ticks) < pdMS_TO_TICKS(CHARGE_EDGE_POLL_MS))
    {
        return;
    }
    s_charge_poll_ticks = now;

    /* The charger loses its register state when external input disappears.
     * Service hot-plug before sampling STAT so a later plug-in can charge. */
    (void)battery_service();

    charging = battery_is_charging();
    if (charging != s_charge_sample)
    {
        s_charge_sample = charging;
        s_charge_agree = 1;
        return;
    }
    if (s_charge_agree < CHARGE_EDGE_STABLE_POLLS)
    {
        s_charge_agree++;
        if (s_charge_agree < CHARGE_EDGE_STABLE_POLLS)
        {
            return;
        }
    }
    if (charging == s_charging_latched)
    {
        return;
    }
    s_charging_latched = charging;
    ESP_LOGI(TAG, "charger %s (SOC %d%%)",
             charging ? "connected" : "disconnected", battery_soc());
    if (!charging)
    {
        return;
    }
    if (s_charge_glimpse_ticks != 0
        && (int32_t)(now - s_charge_glimpse_ticks)
               < pdMS_TO_TICKS(CHARGE_GLIMPSE_MIN_GAP_MS))
    {
        ESP_LOGW(TAG, "charger STAT is flapping; skipping the glimpse");
        return;
    }
    s_charge_glimpse_ticks = now;
    state_lock();
    /* The admin access code must stay readable; every other screen yields. */
    if (s_display_base != DISPLAY_BASE_ADMIN)
    {
        display_show_battery_glimpse_locked();
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

    /*
     * The welcome decoder runs independently. Queue it before the synchronous
     * face loop so its first PCM frame and animation frame zero start together.
     * The asset is provided by yoto_vfs, so an SD/content mount is not needed.
     */
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

    /* Run the visual half of the welcome after queueing its audio half. */
    state_lock();
    display_play_boot_animation_locked();
    /* Show battery state when boot conditions warrant it. */
    s_charging_latched = battery_is_charging();
    if (battery_is_low() || s_charging_latched)
    {
        display_show_battery_glimpse_locked();
    }
    state_unlock();

    boot_require(BOOT_STAGE_NFC, cr95hf_init());
    boot_require(BOOT_STAGE_ENCODER, encoder_init());
    encoder_register_cb(encoder_cb);

    /* Audible volume feedback runs on its own task so a detent never waits for
     * I2S. Losing it costs feedback only, so a failure is not fatal. */
    s_volume_blip_signal = xSemaphoreCreateBinary();
    if (s_volume_blip_signal == NULL
        || xTaskCreate(volume_blip_task, "volume_blip", VOLUME_BLIP_STACK_BYTES,
                       NULL, VOLUME_BLIP_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGW(TAG, "volume feedback task unavailable; knob stays silent");
        s_volume_blip_signal = NULL;
    }

    /* Deferred knob/power gestures run on their own task so they are not stuck
     * behind the main loop's NFC poll. If it cannot start, the main loop still
     * pumps them once per pass — slower, but nothing is lost. */
    s_gesture_signal = xSemaphoreCreateBinary();
    if (s_gesture_signal == NULL
        || xTaskCreate(gesture_task, "gesture", GESTURE_TASK_STACK_BYTES,
                       NULL, GESTURE_TASK_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGW(TAG, "gesture task unavailable; gestures apply on the main loop");
        s_gesture_signal = NULL;
    }
    /* Stock user mode treats a successful FatFS mount as SD availability;
     * its optional higher-level indexes do not gate the mount. */
    esp_err_t content_err = content_init();
    if (content_err != ESP_OK)
    {
        ESP_LOGW(TAG, "SD/content initialization failed: %s",
                 esp_err_to_name(content_err));
    }
    /* These registrations remain installed across admin start/stop cycles. */
    admin_set_code_callback(show_admin_code);
    admin_set_path_callbacks(remote_play_sound, remote_display_image,
                             remote_stop_sound, remote_pause_sound,
                             remote_resume_sound, remote_clear_display);
    admin_set_card_write_callback(remote_write_card);

    err = boot_recovery_clear();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "could not clear boot recovery record: %s",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "boot complete (battery %d%%, %.1f mV)",
             battery_soc(), (double)battery_voltage());
    activity_note();

    uint8_t uid[CR95HF_UID_MAX];
    char url[CR95HF_URL_MAX + 1];
    bool card_present = false;
    uint8_t last_uid[CR95HF_UID_MAX];
    uint8_t last_uid_len = 0;

    while (1)
    {
        /* Deferred encoder gestures first. */
        encoder_actions_pump();

        stack_watch();

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
            activity_note();
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
            activity_note();
            if (admin_is_active())
            {
                admin_set_last_card(NULL, 0, NULL);
            }
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
            if (s_display_base == DISPLAY_BASE_ADMIN && s_admin_code[0] != '\0')
            {
                display_show_access_code(s_admin_code);
            }
            else
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
        /* Edge-polled separately: the latch must track the charger even while
         * the admin code owns the display. */
        charge_edge_check();

        state_lock();
        display_maintain_locked();
        state_unlock();

        idle_power_check();

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
