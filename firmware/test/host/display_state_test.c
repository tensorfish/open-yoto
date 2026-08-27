/*
 * display_state_test.c — host-side deterministic test of the display state
 * machine in firmware/main/app_main.c.
 *
 * The panel cannot be observed from CI, so this test compiles the REAL
 * app_main.c into a plain host program (no ESP-IDF) and drives it through a
 * virtual clock. It reproduces and proves fixed the two regressions:
 *
 *   1. after plugging/unplugging the charger the idle face never came back;
 *   2. the wink face no longer appeared when the right knob was twisted.
 *
 * display_show_rgba() records the frame POINTER of every call, so a test can
 * assert exactly which asset is on the panel by comparing against
 * BOOT_FACE_FRAMES[i], IDLE_FACE_RGBA, WINK_FACE_FRAMES[0]/[1] and
 * BATTERY_ICON_FRAMES[n]. display_draw_volume_overlay() records its volume,
 * and display_clear/display_flush/display_show_access_code count their calls.
 *
 * The real generated headers (boot_face_rgba.h, wink_face_rgba.h,
 * battery_icons_rgba.h) are used as-is via the include path — the frame
 * identities matter. Every other include is a minimal stub under stubs/.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

static int s_failures;

#define CHECK(cond, ...)                                    \
    do                                                      \
    {                                                       \
        if (!(cond))                                        \
        {                                                   \
            fprintf(stderr, "FAIL: ");                      \
            fprintf(stderr, __VA_ARGS__);                   \
            fprintf(stderr, "\n");                          \
            s_failures++;                                   \
        }                                                   \
    } while (0)

#define CONFIG_BOARD_REV_04

/*
 * app_main.c compares int32_t elapsed-ticks against TickType_t (uint32_t) in
 * its debounce and deadline helpers (gesture_debounced, charge_edge_check,
 * battery_periodic_check). The ESP-IDF build does not compile with
 * -Wsign-compare, so scope that one suppression to this include only rather
 * than weakening the flag for the whole suite.
 */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "../../main/app_main.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ---- Virtual clock / FreeRTOS ---- */

static uint32_t s_tick_ms;

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)s_tick_ms;
}

void vTaskDelay(TickType_t ticks)
{
    s_tick_ms += (uint32_t)ticks;
}

/* A healthy fixed margin: stack_watch() must not warn during the host run. */
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 4096;
}

/* xTaskCreate records the entry point but never runs it. */
void (*s_task_entry)(void *);

BaseType_t xTaskCreate(void (*code)(void *), const char *name,
                       uint32_t stack_bytes, void *arg,
                       UBaseType_t priority, TaskHandle_t *handle)
{
    (void)name;
    (void)stack_bytes;
    (void)arg;
    (void)priority;
    s_task_entry = code;
    if (handle != NULL)
    {
        *handle = (TaskHandle_t)1;
    }
    return pdPASS;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return (SemaphoreHandle_t)1;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return (SemaphoreHandle_t)1;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    (void)sem;
    (void)ticks;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    (void)sem;
    return pdTRUE;
}

/* ---- nvs / nvs_flash / esp_system ---- */

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    (void)namespace_name;
    (void)open_mode;
    *out_handle = (nvs_handle_t)1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                       void *out_value, size_t *length)
{
    (void)handle;
    (void)key;
    (void)out_value;
    (void)length;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length)
{
    (void)handle;
    (void)key;
    (void)value;
    (void)length;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    (void)key;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

esp_err_t nvs_flash_init(void)
{
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    return ESP_OK;
}

static int s_restart_count;
esp_reset_reason_t esp_reset_reason(void)
{
    return ESP_RST_POWERON;
}

void esp_restart(void)
{
    s_restart_count++;
}

const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "ESP_ERR";
}

/* ---- battery / audio (test-settable) ---- */

static bool s_charging;
static bool s_low;
static int s_soc = 50;
static float s_voltage = 3700.0f;
static bool s_playing;

bool battery_is_charging(void)
{
    return s_charging;
}

bool battery_is_low(void)
{
    return s_low;
}

int battery_soc(void)
{
    return s_soc;
}

float battery_voltage(void)
{
    return s_voltage;
}

esp_err_t battery_init(void)
{
    return ESP_OK;
}

bool audio_is_playing(void)
{
    return s_playing;
}

bool audio_is_paused(void)
{
    return false;
}

esp_err_t audio_init(void)
{
    return ESP_OK;
}

esp_err_t audio_play(const char *path)
{
    (void)path;
    return ESP_OK;
}

esp_err_t audio_stop(void)
{
    return ESP_OK;
}

esp_err_t audio_pause(void)
{
    return ESP_OK;
}

esp_err_t audio_resume(void)
{
    return ESP_OK;
}

esp_err_t audio_set_volume(int vol)
{
    (void)vol;
    return ESP_OK;
}

esp_err_t audio_play_blip(int freq_hz, int duration_ms)
{
    (void)freq_hz;
    (void)duration_ms;
    return ESP_OK;
}

/* ---- remaining peripherals ---- */

esp_err_t iox_init(void)
{
    return ESP_OK;
}

static bool s_iox_peripherals_powered;
esp_err_t iox_set_peripherals_powered(bool powered)
{
    s_iox_peripherals_powered = powered;
    return ESP_OK;
}

esp_err_t lis2dh12_init(void)
{
    return ESP_OK;
}

esp_err_t cr95hf_init(void)
{
    return ESP_OK;
}

bool cr95hf_poll_card(uint8_t *uid, uint8_t *uid_len, char *url,
                      size_t url_cap, cr95hf_card_info_t *info)
{
    (void)uid;
    (void)uid_len;
    (void)url;
    (void)url_cap;
    (void)info;
    return false;
}

esp_err_t cr95hf_write_url(const char *url, const uint8_t *expected_uid,
                           uint8_t expected_uid_len)
{
    (void)url;
    (void)expected_uid;
    (void)expected_uid_len;
    return ESP_OK;
}

esp_err_t encoder_init(void)
{
    return ESP_OK;
}

void encoder_register_cb(encoder_cb_t cb)
{
    (void)cb;
}

esp_err_t content_init(void)
{
    return ESP_OK;
}

/* Counts every SD-backed lookup, so a test can prove the encoder callback never
 * performs one: doing that on the 4 KB encoder task overflows its stack. */
static int s_content_lookups;
static bool s_admin_active;

int content_get_track_count(const char *url)
{
    (void)url;
    s_content_lookups++;
    return 0;
}

esp_err_t content_get_track(const char *url, int index,
                            char *sound_path, size_t sp)
{
    (void)url;
    (void)index;
    s_content_lookups++;
    snprintf(sound_path, sp, "/sdcard/media/track%d.mp3", index);
    return ESP_OK;
}

esp_err_t content_get_track_image(const char *url, int index,
                                  char *image_path, size_t ip)
{
    (void)url;
    (void)index;
    s_content_lookups++;
    snprintf(image_path, ip, "/sdcard/media/track%d.img", index);
    return ESP_OK;
}

esp_err_t admin_start(char *code_out, size_t code_size)
{
    (void)code_out;
    (void)code_size;
    return ESP_OK;
}

esp_err_t admin_stop(void)
{
    return ESP_OK;
}

bool admin_is_active(void)
{
    return s_admin_active;
}

void admin_set_last_card(const uint8_t *uid, uint8_t uid_len,
                         const char *url)
{
    (void)uid;
    (void)uid_len;
    (void)url;
}

void admin_set_code_callback(admin_code_cb_t cb)
{
    (void)cb;
}

void admin_set_card_write_callback(admin_card_write_cb_t cb)
{
    (void)cb;
}

void admin_set_path_callbacks(admin_path_cb_t play_sound,
                              admin_path_cb_t display_image,
                              admin_action_cb_t stop_sound,
                              admin_action_cb_t pause_sound,
                              admin_action_cb_t resume_sound,
                              admin_action_cb_t clear_display)
{
    (void)play_sound;
    (void)display_image;
    (void)stop_sound;
    (void)pause_sound;
    (void)resume_sound;
    (void)clear_display;
}

esp_err_t yoto_vfs_init(void)
{
    return ESP_OK;
}

/* ---- display recording ---- */

#define MAX_FRAMES 8192
static const uint8_t *s_frames[MAX_FRAMES];
/* Bar-draw count at the instant the frame was drawn, and whether the volume
 * overlay was live then. Together these decide whether the bar was re-applied
 * over that frame: a bar draw landed between frame i and frame i+1 exactly
 * when the watermark advanced. */
static int s_frame_bar_mark[MAX_FRAMES];
static bool s_frame_overlay_live[MAX_FRAMES];
static int s_frame_count;

#define MAX_VOLUME_DRAWS 1024
static int s_volume_draws[MAX_VOLUME_DRAWS];
static int s_volume_draw_count;

/* Counts are globals (external linkage) so recording them is never flagged as
 * an unused-but-set variable even when a given test does not assert on them. */
int s_clear_count;
int s_flush_count;
int s_access_code_count;

esp_err_t display_init(void)
{
    return ESP_OK;
}

void display_show_rgba(const uint8_t rgba[16 * 16 * 4])
{
    if (s_frame_count < MAX_FRAMES)
    {
        s_frame_bar_mark[s_frame_count] = s_volume_draw_count;
        s_frame_overlay_live[s_frame_count] = s_volume_overlay_deadline != 0;
        s_frames[s_frame_count++] = rgba;
    }
}

void display_draw_volume_overlay(int volume)
{
    if (s_volume_draw_count < MAX_VOLUME_DRAWS)
    {
        s_volume_draws[s_volume_draw_count++] = volume;
    }
}

void display_clear(void)
{
    s_clear_count++;
}

void display_flush(void)
{
    s_flush_count++;
}

void display_show_access_code(
    const char code[DISPLAY_ACCESS_CODE_LEN + 1])
{
    (void)code;
    s_access_code_count++;
}

void display_set_pixel(int x, int y, bool on)
{
    (void)x;
    (void)y;
    (void)on;
}

esp_err_t display_show_rgb56516(const uint16_t pixels[16 * 16])
{
    (void)pixels;
    return ESP_OK;
}

esp_err_t display_color64_begin(void)
{
    return ESP_OK;
}

esp_err_t display_color64_write_row(uint8_t y, const uint16_t pixels[64])
{
    (void)y;
    (void)pixels;
    return ESP_OK;
}

esp_err_t display_color64_end(void)
{
    return ESP_OK;
}

/* ---- test helpers ---- */

static const uint8_t *last_frame(void)
{
    return s_frame_count > 0 ? s_frames[s_frame_count - 1] : NULL;
}

static void set_charging(bool v)
{
    s_charging = v;
}


/*
 * Advance the virtual clock by `ms`, running the same per-pass sequence the
 * firmware main loop does — charge_edge_check() then display_maintain_locked()
 * under the state mutex — on a 100 ms cadence. Each tick is 1 ms.
 */
static void advance_ms(uint32_t ms)
{
    while (ms >= 100)
    {
        s_tick_ms += 100;
        charge_edge_check();
        state_lock();
        display_maintain_locked();
        state_unlock();
        ms -= 100;
    }
    if (ms != 0)
    {
        s_tick_ms += ms;
    }
}

/*
 * Reset every piece of static firmware state plus the recording buffers to a
 * known, deterministic starting point: base IDLE, nothing playing, no charger
 * edges, battery healthy and not charging.
 */
static void reset_state(void)
{
    s_tick_ms = 0;

    s_charging = false;
    s_low = false;
    s_soc = 50;
    s_voltage = 3700.0f;
    s_playing = false;

    s_display_base = DISPLAY_BASE_IDLE;
    s_display_image_path[0] = '\0';
    s_admin_code[0] = '\0';
    s_battery_visual_deadline = 0;
    s_volume_overlay_deadline = 0;
    s_idle_wink_deadline = 0;
    s_volume_draw_due = 0;
    s_volume_dirty = false;
    s_wink_frame_index = 0;
    s_charging_latched = false;
    s_charge_poll_ticks = 0;
    s_charge_glimpse_ticks = 0;
    s_charge_sample = false;
    s_charge_agree = 0;

    s_volume = 70;
    s_track_count = 0;
    s_track_index = 0;
    s_powered_off = false;
    s_current_url[0] = '\0';
    s_volume_blip_signal = NULL;
    s_last_playpause_ticks = 0;
    s_last_power_ticks = 0;
    s_battery_check_ticks = 0;
    s_pending_skip = 0;
    s_pending_power = false;
    s_pending_screen_toggle = false;
    s_power_resume_magic = 0;
    s_content_lookups = 0;
    s_admin_active = false;
    s_restart_count = 0;
    s_iox_peripherals_powered = true;

    s_state_mutex = xSemaphoreCreateMutex();

    s_frame_count = 0;
    s_volume_draw_count = 0;
    s_clear_count = 0;
    s_flush_count = 0;
    s_access_code_count = 0;
}

/* Fire one encoder event exactly as the encoder task would. */
static void encoder_event(int id, int delta, encoder_event_t event)
{
    encoder_cb(id, delta, event);
}

int main(void)
{
    /* ---------------------------------------------------- 1. BOOT --------- */
    {
        uint32_t start;

        reset_state();
        start = s_tick_ms;
        state_lock();
        display_play_boot_animation_locked();
        state_unlock();

        CHECK(s_frame_count == BOOT_FACE_FRAME_COUNT,
              "boot: expected %d frames, drew %d",
              BOOT_FACE_FRAME_COUNT, s_frame_count);
        for (int i = 0; i < BOOT_FACE_FRAME_COUNT - 1; i++)
        {
            CHECK(s_frames[i] == BOOT_FACE_FRAMES[i],
                  "boot: frame %d is not BOOT_FACE_FRAMES[%d]", i, i);
        }
        CHECK(BOOT_FACE_FRAMES[BOOT_FACE_FRAME_COUNT - 1] == IDLE_FACE_RGBA,
              "boot: frame 8 (BOOT_FACE_FRAMES[7]) must be IDLE_FACE_RGBA");
        CHECK(s_frames[BOOT_FACE_FRAME_COUNT - 1] == IDLE_FACE_RGBA,
              "boot: final drawn frame must be the resting face IDLE_FACE_RGBA");
        CHECK(s_tick_ms - start == 500u,
              "boot: animation took %u ms, expected 500 ms (16 fps)",
              (unsigned)(s_tick_ms - start));
        CHECK(s_display_base == DISPLAY_BASE_IDLE,
              "boot: base should be DISPLAY_BASE_IDLE, got %d",
              (int)s_display_base);
    }

    /* --------------------------------- 2. CHARGE GLIMPSE RETURNS TO FACE --- */
    {
        int mark;
        bool saw_charging = false;

        reset_state();
        state_lock();
        display_set_idle_locked();
        state_unlock();

        set_charging(true);
        mark = s_frame_count;
        /* Four stable 500 ms polls confirm the edge and arm the glimpse. */
        advance_ms(CHARGE_EDGE_POLL_MS * CHARGE_EDGE_STABLE_POLLS + 100);

        for (int i = mark; i < s_frame_count; i++)
        {
            if (s_frames[i] == battery_charging_icon_for(s_soc))
            {
                saw_charging = true;
            }
        }
        CHECK(saw_charging,
              "charge glimpse: no charging-icon frame drawn after plug-in");

        advance_ms(BATTERY_GLIMPSE_MS + 200);
        CHECK(last_frame() == IDLE_FACE_RGBA,
              "charge glimpse: idle face not restored within %d ms",
              BATTERY_GLIMPSE_MS + 200);
        CHECK(s_display_base == DISPLAY_BASE_IDLE,
              "charge glimpse: base should be IDLE, got %d",
              (int)s_display_base);
    }

    /* ----------------------------------------- 3. FLAPPING STAT ----------- */
    {
        int mark;
        int glimpse_count = 0;

        reset_state();
        state_lock();
        display_set_idle_locked();
        state_unlock();

        mark = s_frame_count;
        /*
         * Flap the charger STAT line for 30 s of virtual time, flipping the
         * sampled level on the 500 ms poll cadence (each level held for two
         * polls, a ~1 s blink). With the debounce (4 stable polls) and the
         * 30 s minimum gap in place, this must not re-arm the glimpse.
         */
        for (int poll = 0; poll < 60; poll++)
        {
            set_charging((poll / 2) % 2 == 0);
            advance_ms(CHARGE_EDGE_POLL_MS);
        }

        for (int i = mark; i < s_frame_count; i++)
        {
            if (s_frames[i] == battery_charging_icon_for(s_soc))
            {
                glimpse_count++;
            }
        }
        CHECK(glimpse_count <= 1,
              "flapping STAT: %d battery glimpse(s) in 30 s — a blinking "
              "charger STAT must not own the display", glimpse_count);
        CHECK(last_frame() == IDLE_FACE_RGBA,
              "flapping STAT: the idle face is not on the panel at the end");
        CHECK(s_display_base == DISPLAY_BASE_IDLE,
              "flapping STAT: base should be IDLE, got %d",
              (int)s_display_base);
    }

    /* ------------------------------------------- 4. WINK (regression 2) --- */
    {
        reset_state();
        state_lock();
        display_set_idle_locked();
        state_unlock();

        for (int i = 0; i < 4; i++)
        {
            skip_track(1);
            CHECK(last_frame() == WINK_FACE_FRAMES[i % 2],
                  "wink: turn %d should draw WINK_FACE_FRAMES[%d]",
                  i + 1, i % 2);
            advance_ms(400);
            CHECK(last_frame() == IDLE_FACE_RGBA,
                  "wink: idle face not restored after turn %d "
                  "(within IDLE_WINK_MS)", i + 1);
        }
    }

    /* ----------------------------------------- 5. WINK OVER BATTERY ------- */
    {
        reset_state();
        set_charging(true);
        state_lock();
        display_set_idle_locked();
        display_show_battery_glimpse_locked();
        state_unlock();

        CHECK(s_battery_visual_deadline != 0,
              "wink-over-battery: glimpse should arm s_battery_visual_deadline");
        CHECK(last_frame() == battery_charging_icon_for(s_soc),
              "wink-over-battery: glimpse should show the charging icon");

        skip_track(1);
        CHECK(last_frame() == WINK_FACE_FRAMES[0],
              "wink-over-battery: wink must draw over the battery glimpse");

        advance_ms(IDLE_WINK_MS + 100);
        CHECK(last_frame() == IDLE_FACE_RGBA,
              "wink-over-battery: after the wink the face must return, "
              "never the battery icon");
        CHECK(s_display_base == DISPLAY_BASE_IDLE,
              "wink-over-battery: base should be IDLE, got %d",
              (int)s_display_base);
    }

    /* ----------------------------------------- 6. WINK PRECEDENCE --------- */
    {
        int mark;

        reset_state();

        s_display_base = DISPLAY_BASE_CARD;
        mark = s_frame_count;
        skip_track(1);
        CHECK(s_frame_count == mark,
              "wink precedence: skip_track must draw nothing over card art");

        s_display_base = DISPLAY_BASE_ADMIN;
        mark = s_frame_count;
        skip_track(1);
        CHECK(s_frame_count == mark,
              "wink precedence: skip_track must draw nothing over admin code");

        s_display_base = DISPLAY_BASE_CARD;
        mark = s_frame_count;
        display_show_wink_locked();
        CHECK(s_frame_count == mark,
              "wink precedence: display_show_wink_locked must draw nothing "
              "over CARD");

        s_display_base = DISPLAY_BASE_ADMIN;
        mark = s_frame_count;
        display_show_wink_locked();
        CHECK(s_frame_count == mark,
              "wink precedence: display_show_wink_locked must draw nothing "
              "over ADMIN");
    }

    /* ---------------------------------------- 7. VOLUME COMPOSITION ------- */
    {
        int d0;
        int d1;
        int d2;
        int live_frames = 0;
        int lost_bars = 0;

        reset_state();
        state_lock();
        display_set_idle_locked();
        s_volume = 40;
        display_show_volume_locked();
        s_volume = 55;
        display_show_volume_locked();
        s_volume = 75;
        display_show_volume_locked();
        state_unlock();

        /* (a) one coalesced bar draw per 100 ms window, with the newest level.
         * Three detents inside one window must collapse to a single draw. */
        d0 = s_volume_draw_count;
        advance_ms(VOLUME_DRAW_COALESCE_MS);
        CHECK(s_volume_draw_count - d0 == 1,
              "volume: expected exactly 1 coalesced bar draw, got %d",
              s_volume_draw_count - d0);
        CHECK(s_volume_draws[s_volume_draw_count - 1] == 75,
              "volume: bar must reflect the latest volume 75, got %d",
              s_volume_draws[s_volume_draw_count - 1]);

        /* A second window with a fresh level draws exactly one more bar. */
        d1 = s_volume_draw_count;
        state_lock();
        s_volume = 20;
        display_show_volume_locked();
        state_unlock();
        advance_ms(VOLUME_DRAW_COALESCE_MS);
        CHECK(s_volume_draw_count - d1 == 1,
              "volume: second window expected exactly 1 bar draw, got %d",
              s_volume_draw_count - d1);
        CHECK(s_volume_draws[s_volume_draw_count - 1] == 20,
              "volume: bar must reflect the latest volume 20, got %d",
              s_volume_draws[s_volume_draw_count - 1]);

        /* Drive several icon frames while the overlay stays live: two winks
         * (each with its expiry repaint) and a charging glimpse. */
        s_track_count = 0;
        skip_track(1);
        CHECK(last_frame() == WINK_FACE_FRAMES[0],
              "volume: wink frame expected while the overlay is live");
        advance_ms(IDLE_WINK_MS + 100);
        state_lock();
        display_show_volume_locked();
        state_unlock();
        skip_track(1);
        CHECK(last_frame() == WINK_FACE_FRAMES[1],
              "volume: second wink frame expected while the overlay is live");
        advance_ms(IDLE_WINK_MS + 100);
        state_lock();
        display_show_volume_locked();
        state_unlock();
        set_charging(true);
        state_lock();
        display_show_battery_glimpse_locked();
        state_unlock();
        CHECK(last_frame() == battery_charging_icon_for(s_soc),
              "volume: charging glimpse expected while the overlay is live");

        /* (b) every icon frame drawn while the overlay was live is immediately
         * followed by a bar draw — the bar is re-applied, never lost. */
        for (int i = 0; i < s_frame_count; i++)
        {
            int after = (i + 1 < s_frame_count) ? s_frame_bar_mark[i + 1]
                                                : s_volume_draw_count;

            if (!s_frame_overlay_live[i])
            {
                continue;
            }
            live_frames++;
            if (after <= s_frame_bar_mark[i])
            {
                lost_bars++;
            }
        }
        CHECK(live_frames >= 5,
              "volume: only %d frame(s) drawn while the overlay was live; the "
              "invariant below would be vacuous", live_frames);
        CHECK(lost_bars == 0,
              "volume: %d of %d frame(s) drawn under a live overlay were not "
              "followed by a bar draw (the bar was lost)",
              lost_bars, live_frames);

        /* (c) after VOLUME_OVERLAY_MS the base is repainted and the bar stops.
         * The glimpse is still live, so let it expire back to the face first. */
        set_charging(false);
        advance_ms(VOLUME_OVERLAY_MS + BATTERY_GLIMPSE_MS + 200);
        d2 = s_volume_draw_count;
        advance_ms(VOLUME_OVERLAY_MS);
        CHECK(s_volume_overlay_deadline == 0 && s_volume_draw_due == 0
              && !s_volume_dirty,
              "volume: the expired overlay must be fully cancelled");
        CHECK(s_volume_draw_count == d2,
              "volume: no further bar draws after the overlay expires "
              "(%d extra)", s_volume_draw_count - d2);
        CHECK(last_frame() == IDLE_FACE_RGBA,
              "volume: base must be repainted to the idle face after expiry");
        CHECK(s_display_base == DISPLAY_BASE_IDLE,
              "volume: base should be IDLE after expiry, got %d",
              (int)s_display_base);
    }

    /* ---------------------------------------- 8. BATTERY ICON MAPPING ----- */
    {
        static const int socs[] =
            { 0, 1, 10, 11, 20, 21, 50, 90, 91, 99, 100 };
        static const int expected[] =
            { 1, 1, 1, 2, 2, 3, 5, 9, 10, 10, 10 };

        for (size_t i = 0; i < sizeof(socs) / sizeof(socs[0]); i++)
        {
            CHECK(battery_icon_for(socs[i]) == BATTERY_ICON_FRAMES[expected[i]],
                  "battery icon: soc %d should map to BATTERY_ICON_FRAMES[%d]",
                  socs[i], expected[i]);
        }
        CHECK(battery_icon_for(-1) == BATTERY_ICON_EMPTY,
              "battery icon: soc -1 should map to BATTERY_ICON_EMPTY");

        /*
         * Charging maps each ten-point band to its charging frame:
         * battery-charging-0.png for 0..9%, battery-charging-10.png for
         * 10..19%, ... battery-charging-100.png at 100%; unknown reads as 0%.
         */
        {
            static const int socs[] =
                { 0, 9, 10, 19, 20, 50, 90, 99, 100, -1 };
            static const int expected[] =
                { 0, 0, 1, 1, 2, 5, 9, 9, 10, 0 };

            for (size_t i = 0; i < sizeof(socs) / sizeof(socs[0]); i++)
            {
                CHECK(battery_charging_icon_for(socs[i]) ==
                          BATTERY_ICON_FRAMES[11 + expected[i]],
                      "charging icon: soc %d should map to frame 11+%d",
                      socs[i], expected[i]);
            }
        }
        set_charging(true);
        CHECK(battery_icon_now() == battery_charging_icon_for(s_soc),
              "battery icon: charging must map to battery_charging_icon_for");
    }

    /* ------------------------------------------- 9. POWER OFF / ON -------- */
    {
        int restart_mark;

        reset_state();
        power_toggle();
        CHECK(s_powered_off, "power: the first hold must switch off");
        CHECK(s_display_base == DISPLAY_BASE_OFF,
              "power: off should park the base at OFF, got %d",
              (int)s_display_base);
        CHECK(s_clear_count > 0 && s_flush_count > 0,
              "power: off must blank the panel");
        CHECK(!s_iox_peripherals_powered,
              "power: off must disconnect downstream peripheral rails");

        restart_mark = s_restart_count;
        power_toggle();
        CHECK(!s_powered_off, "power: the second hold must switch on");
        CHECK(s_iox_peripherals_powered,
              "power: on must restore downstream peripheral rails");
        CHECK(s_power_resume_magic == POWER_RESUME_MAGIC,
              "power: on must mark the restart as a low-power resume");
        CHECK(s_restart_count == restart_mark + 1,
              "power: on must restart to cold-initialize every peripheral");
    }

    /* ------------------------------ 10. ENCODER DOES NO HEAVY WORK -------- */
    {
        int lookups;

        reset_state();
        advance_ms(200);

        /* A right-knob turn with a multi-track card loaded must NOT read the SD
         * card from the encoder callback: that work needs more stack than the
         * 4 KB encoder task has, which is what panicked the device. */
        s_track_count = 3;
        snprintf(s_current_url, sizeof(s_current_url), "%s",
                 "https://example.com/card");
        s_content_lookups = 0;
        encoder_event(ENCODER_ID_1, 1, ENCODER_EVT_TURN);
        CHECK(s_content_lookups == 0,
              "encoder: the callback performed %d SD lookup(s); track skipping "
              "must be deferred to the main loop", s_content_lookups);
        CHECK(s_pending_skip == 1,
              "encoder: the callback must record the skip, got %d",
              s_pending_skip);

        /* The main loop then performs it. */
        encoder_actions_pump();
        lookups = s_content_lookups;
        CHECK(lookups > 0,
              "encoder: the pump must perform the deferred skip");
        CHECK(s_pending_skip == 0,
              "encoder: the pump must clear the pending skip");
        CHECK(s_track_index == 1,
              "encoder: the deferred skip must advance the track, got %d",
              s_track_index);

        /* A short power press changes only the screen and restores a battery
         * status frame when pressed again. */
        s_last_power_ticks = 0;
        encoder_event(ENCODER_ID_POWER, 0, ENCODER_EVT_SHORT_PRESS);
        CHECK(s_pending_screen_toggle && !s_powered_off,
              "encoder: a power tap must queue a screen toggle");
        encoder_actions_pump();
        CHECK(s_display_base == DISPLAY_BASE_OFF && !s_powered_off,
              "encoder: a power tap must blank only the screen");
        encoder_event(ENCODER_ID_POWER, 0, ENCODER_EVT_SHORT_PRESS);
        encoder_actions_pump();
        CHECK(s_display_base == DISPLAY_BASE_IDLE,
              "encoder: a second tap must restore the idle base");
        CHECK(last_frame() == BATTERY_ICON_FRAMES[5],
              "encoder: screen-on must show the current battery percentage");

        encoder_event(ENCODER_ID_POWER, 0, ENCODER_EVT_LONG_PRESS);
        CHECK(s_pending_power && !s_powered_off,
              "encoder: a power hold must be recorded, not executed inline");
        encoder_actions_pump();
        CHECK(s_powered_off,
              "encoder: the pump must apply the deferred power transition");

        /* A skip captured for one card must not be applied to a different card
         * that arrived in between: the turn belonged to the old story. */
        reset_state();
        s_track_count = 3;
        s_track_index = 0;
        snprintf(s_current_url, sizeof(s_current_url), "%s",
                 "https://example.com/first");
        encoder_event(ENCODER_ID_1, 1, ENCODER_EVT_TURN);
        CHECK(s_pending_skip == 1, "encoder: skip must be recorded for the card");
        /* The main loop swaps the card before the gesture task runs. */
        snprintf(s_current_url, sizeof(s_current_url), "%s",
                 "https://example.com/second");
        s_content_lookups = 0;
        encoder_actions_pump();
        CHECK(s_track_index == 0 && s_content_lookups == 0,
              "encoder: a skip captured for another card must be dropped "
              "(track_index %d, lookups %d)", s_track_index, s_content_lookups);
        CHECK(s_pending_skip == 0,
              "encoder: the dropped skip must still be cleared");
    }

    /* ------------------------------ 11. ADMIN CODE STAYS ON THE PANEL ----- */
    {
        int codes;

        reset_state();
        advance_ms(200);

        /* Admin mode active with a code displayed. */
        s_admin_active = true;
        snprintf(s_admin_code, sizeof(s_admin_code), "%s", "ABC123");
        state_lock();
        s_display_base = DISPLAY_BASE_ADMIN;
        state_unlock();

        /* Every "back to the face" path must land on the code instead. */
        codes = s_access_code_count;
        state_lock();
        display_set_idle_locked();
        state_unlock();
        CHECK(s_access_code_count == codes + 1,
              "admin: display_set_idle_locked must redraw the access code");
        CHECK(s_display_base == DISPLAY_BASE_ADMIN,
              "admin: the base must stay ADMIN, got %d", (int)s_display_base);

        /* A card image shown from the web UI, then the card is removed: the
         * panel must return to the code, never to the face. */
        codes = s_access_code_count;
        state_lock();
        s_display_base = DISPLAY_BASE_CARD;
        display_set_idle_locked();
        state_unlock();
        CHECK(s_access_code_count == codes + 1 && last_frame() != IDLE_FACE_RGBA,
              "admin: after card removal the code must return, not the face");

        /* A right-knob twist must not replace the code with a wink. */
        codes = s_access_code_count;
        s_track_count = 0;
        state_lock();
        display_show_wink_locked();
        state_unlock();
        CHECK(last_frame() != WINK_FACE_FRAMES[0]
              && last_frame() != WINK_FACE_FRAMES[1],
              "admin: a wink must not cover the access code");
        /* Remote image display may temporarily replace the code in admin mode;
         * this host fixture has no SD image, so the open fails without
         * repainting the access code. Clear must restore the code successfully. */
        codes = s_access_code_count;
        CHECK(remote_display_image("/sdcard/media/cover.img")
              == ESP_ERR_NOT_FOUND && s_access_code_count == codes,
              "admin: failed remote image must not repaint the access code");
        codes = s_access_code_count;
        CHECK(remote_clear_display() == ESP_OK
              && s_access_code_count == codes + 1,
              "admin: remote clear must restore the access code");

        /* And once admin stops, the face comes back. */
        s_admin_active = false;
        s_admin_code[0] = '\0';
        state_lock();
        display_set_idle_locked();
        state_unlock();
        CHECK(last_frame() == IDLE_FACE_RGBA,
              "admin: stopping admin must restore the idle face");
    }

    if (s_failures != 0)
    {
        fprintf(stderr, "display state test: %d assertion(s) failed\n",
                s_failures);
        return 1;
    }

    printf("display state test: all assertions passed\n");
    return 0;
}
