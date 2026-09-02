#include "bluetooth.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "audio.h"
#include "esp_a2dp_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define BLUETOOTH_DEVICE_NAME              "Open Yoto"
#define BT_PROFILE_TIMEOUT_MS              5000
#define BT_DISCONNECT_TIMEOUT_MS           3000
#define BT_WORKER_QUIESCE_TIMEOUT_MS       3000
#define BT_WORKER_STACK_BYTES              4096
#define BT_WORKER_PRIORITY                 5
#define BT_PCM_RING_SLOTS                  4
#define BT_SBC_MAX_FRAMES_PER_PACKET       15
#define BT_SBC_MAX_SAMPLES_PER_FRAME       128
#define BT_SBC_MAX_CHANNELS                2
#define BT_PCM_SLOT_SAMPLES                (BT_SBC_MAX_FRAMES_PER_PACKET * \
                                            BT_SBC_MAX_SAMPLES_PER_FRAME * \
                                            BT_SBC_MAX_CHANNELS)
#define BT_OVERFLOW_WARNING_INTERVAL_MS    5000
#define BT_CANCEL_GATE_CLOSED              UINT32_C(0x80000000)
#define BT_CANCEL_GATE_REFS                UINT32_C(0x7fffffff)

#define BT_EVENT_A2DP_INIT                 BIT0
#define BT_EVENT_A2DP_DEINIT               BIT1
#define BT_EVENT_DISCONNECTED              BIT2
#define BT_EVENT_WORKER_QUIESCENT          BIT3

static const char *TAG = "bluetooth";

typedef enum
{
    BT_LIFECYCLE_UNINITIALIZED = 0,
    BT_LIFECYCLE_INITIALIZING,
    BT_LIFECYCLE_RUNNING,
    BT_LIFECYCLE_STOPPING,
    BT_LIFECYCLE_CLEANUP_REQUIRED,
} bluetooth_lifecycle_t;

typedef struct
{
    uint32_t epoch;
    uint32_t len;
    int16_t samples[BT_PCM_SLOT_SAMPLES];
} pcm_slot_t;

static StaticEventGroup_t s_event_storage;
static EventGroupHandle_t s_events;
static StaticTask_t s_worker_task_storage;
static StackType_t s_worker_stack[BT_WORKER_STACK_BYTES];
static TaskHandle_t s_worker_task;
static pcm_slot_t s_pcm_ring[BT_PCM_RING_SLOTS];

static atomic_uint s_lifecycle = ATOMIC_VAR_INIT(BT_LIFECYCLE_UNINITIALIZED);
static atomic_uint s_callbacks_active = ATOMIC_VAR_INIT(0);
static atomic_uint s_shutting_down = ATOMIC_VAR_INIT(0);
static atomic_uint s_connected = ATOMIC_VAR_INIT(0);
static atomic_uint s_sink_requested = ATOMIC_VAR_INIT(0);
static atomic_uint s_sink_ready = ATOMIC_VAR_INIT(0);
static atomic_uint s_stream_config = ATOMIC_VAR_INIT(0);
static atomic_uint s_epoch_counter = ATOMIC_VAR_INIT(0);
static atomic_uint s_stream_command = ATOMIC_VAR_INIT(0);
static atomic_uint s_active_epoch = ATOMIC_VAR_INIT(0);
static atomic_uint s_active_channels = ATOMIC_VAR_INIT(0);
static atomic_uint s_accepting_pcm = ATOMIC_VAR_INIT(0);
static atomic_uint s_audio_token = ATOMIC_VAR_INIT(0);
static atomic_uint s_cancel_gate = ATOMIC_VAR_INIT(BT_CANCEL_GATE_CLOSED);
static atomic_uint s_ring_write = ATOMIC_VAR_INIT(0);
static atomic_uint s_ring_read = ATOMIC_VAR_INIT(0);
static atomic_uint s_overflow_drops = ATOMIC_VAR_INIT(0);

static portMUX_TYPE s_peer_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_bd_addr_t s_peer_address;

/* These flags are serialized by bluetooth_init()/bluetooth_stop(). */
static bool s_ble_memory_released;
static bool s_controller_initialized;
static bool s_controller_enabled;
static bool s_bluedroid_initialized;
static bool s_bluedroid_enabled;

static void bluetooth_worker(void *arg);

static void notify_worker(void)
{
    TaskHandle_t task = s_worker_task;
    if (task != NULL)
    {
        xTaskNotifyGive(task);
    }
}

static esp_err_t ensure_runtime(void)
{
    if (s_events == NULL)
    {
        s_events = xEventGroupCreateStatic(&s_event_storage);
        if (s_events == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_worker_task == NULL)
    {
        s_worker_task = xTaskCreateStatic(bluetooth_worker, "bt_pcm",
                                          BT_WORKER_STACK_BYTES, NULL,
                                          BT_WORKER_PRIORITY, s_worker_stack,
                                          &s_worker_task_storage);
        if (s_worker_task == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static uint32_t next_epoch(void)
{
    uint32_t epoch = atomic_fetch_add_explicit(&s_epoch_counter, 1,
                                                memory_order_relaxed) + 1;
    epoch &= BT_CANCEL_GATE_REFS;
    if (epoch == 0)
    {
        epoch = atomic_fetch_add_explicit(&s_epoch_counter, 1,
                                           memory_order_relaxed) + 1;
        epoch &= BT_CANCEL_GATE_REFS;
    }
    return epoch;
}

static bool acquire_cancel_reference(void)
{
    uint32_t gate = atomic_load_explicit(&s_cancel_gate,
                                          memory_order_acquire);
    while ((gate & BT_CANCEL_GATE_CLOSED) == 0)
    {
        if ((gate & BT_CANCEL_GATE_REFS) == BT_CANCEL_GATE_REFS)
        {
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&s_cancel_gate, &gate,
                                                   gate + 1,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire))
        {
            return true;
        }
    }
    return false;
}

static void cancel_active_audio(void)
{
    if (!acquire_cancel_reference())
    {
        return;
    }

    audio_pcm_stream_t token =
        (audio_pcm_stream_t)atomic_load_explicit(&s_audio_token,
                                                  memory_order_acquire);
    audio_pcm_stream_cancel(token);
    atomic_fetch_sub_explicit(&s_cancel_gate, 1, memory_order_release);
}

static void request_stream_stop(void)
{
    atomic_store_explicit(&s_accepting_pcm, 0, memory_order_release);
    uint32_t epoch = next_epoch();
    atomic_store_explicit(&s_stream_command, epoch << 1,
                          memory_order_release);
    cancel_active_audio();
    notify_worker();
}

static void request_stream_start(void)
{
    uint32_t config = atomic_load_explicit(&s_stream_config,
                                            memory_order_acquire);
    if (config == 0)
    {
        ESP_LOGW(TAG, "ignoring A2DP start without a valid SBC configuration");
        request_stream_stop();
        return;
    }

    atomic_store_explicit(&s_accepting_pcm, 0, memory_order_release);
    uint32_t epoch = next_epoch();
    atomic_store_explicit(&s_stream_command, (epoch << 1) | 1,
                          memory_order_release);
    cancel_active_audio();
    xEventGroupClearBits(s_events, BT_EVENT_WORKER_QUIESCENT);
    notify_worker();
}

static void ring_drop_all(void)
{
    uint32_t write = atomic_load_explicit(&s_ring_write,
                                           memory_order_acquire);
    atomic_store_explicit(&s_ring_read, write, memory_order_release);
}

static void worker_close_stream(audio_pcm_stream_t token)
{
    atomic_store_explicit(&s_accepting_pcm, 0, memory_order_release);
    atomic_fetch_or_explicit(&s_cancel_gate, BT_CANCEL_GATE_CLOSED,
                             memory_order_acq_rel);
    while ((atomic_load_explicit(&s_cancel_gate, memory_order_acquire)
            & BT_CANCEL_GATE_REFS) != 0)
    {
        vTaskDelay(1);
    }

    audio_pcm_stream_cancel(token);
    esp_err_t err = audio_pcm_stream_close(token);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "audio PCM stream close failed: %s",
                 esp_err_to_name(err));
    }
    atomic_store_explicit(&s_active_channels, 0, memory_order_release);
    atomic_store_explicit(&s_active_epoch, 0, memory_order_release);
    ring_drop_all();
}

static void worker_report_overflow(uint32_t *pending, TickType_t *last_warning,
                                   bool *has_warned)
{
    uint32_t dropped = atomic_exchange_explicit(&s_overflow_drops, 0,
                                                 memory_order_acq_rel);
    if (UINT32_MAX - *pending < dropped)
    {
        *pending = UINT32_MAX;
    }
    else
    {
        *pending += dropped;
    }

    if (*pending == 0)
    {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t interval = pdMS_TO_TICKS(BT_OVERFLOW_WARNING_INTERVAL_MS);
    if (!*has_warned || (TickType_t)(now - *last_warning) >= interval)
    {
        ESP_LOGW(TAG, "dropped %lu complete PCM buffer(s): ring full or buffer too large",
                 (unsigned long)*pending);
        *pending = 0;
        *last_warning = now;
        *has_warned = true;
    }
}

static TickType_t worker_wait_time(uint32_t pending, TickType_t last_warning,
                                   bool has_warned)
{
    if (pending == 0 || !has_warned)
    {
        return portMAX_DELAY;
    }

    TickType_t interval = pdMS_TO_TICKS(BT_OVERFLOW_WARNING_INTERVAL_MS);
    TickType_t elapsed = xTaskGetTickCount() - last_warning;
    return elapsed >= interval ? 0 : interval - elapsed;
}

static void bluetooth_worker(void *arg)
{
    (void)arg;
    audio_pcm_stream_t token = 0;
    uint32_t active_command = 0;
    uint8_t channels = 0;
    bool stream_open = false;
    uint32_t pending_drops = 0;
    TickType_t last_warning = 0;
    bool has_warned = false;

    xEventGroupSetBits(s_events, BT_EVENT_WORKER_QUIESCENT);

    for (;;)
    {
        worker_report_overflow(&pending_drops, &last_warning, &has_warned);

        uint32_t command = atomic_load_explicit(&s_stream_command,
                                                 memory_order_acquire);
        if (stream_open && command != active_command)
        {
            worker_close_stream(token);
            stream_open = false;
            active_command = 0;
            channels = 0;
            xEventGroupSetBits(s_events, BT_EVENT_WORKER_QUIESCENT);
            continue;
        }

        if (!stream_open && (command & 1) != 0)
        {
            uint32_t config = atomic_load_explicit(&s_stream_config,
                                                    memory_order_acquire);
            uint32_t rate = config >> 8;
            uint8_t requested_channels = (uint8_t)(config & 0xff);
            uint32_t expected = command;

            xEventGroupClearBits(s_events, BT_EVENT_WORKER_QUIESCENT);
            ring_drop_all();
            if (rate == 0 || (requested_channels != 1 && requested_channels != 2))
            {
                (void)atomic_compare_exchange_strong_explicit(
                    &s_stream_command, &expected, command & ~UINT32_C(1),
                    memory_order_acq_rel, memory_order_acquire);
                xEventGroupSetBits(s_events, BT_EVENT_WORKER_QUIESCENT);
                continue;
            }

            esp_err_t err = audio_pcm_stream_open(rate, requested_channels,
                                                   &token);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "audio PCM stream open failed: %s",
                         esp_err_to_name(err));
                expected = command;
                (void)atomic_compare_exchange_strong_explicit(
                    &s_stream_command, &expected, command & ~UINT32_C(1),
                    memory_order_acq_rel, memory_order_acquire);
                xEventGroupSetBits(s_events, BT_EVENT_WORKER_QUIESCENT);
                continue;
            }

            if (atomic_load_explicit(&s_stream_command,
                                     memory_order_acquire) != command)
            {
                audio_pcm_stream_cancel(token);
                err = audio_pcm_stream_close(token);
                if (err != ESP_OK)
                {
                    ESP_LOGW(TAG, "stale audio PCM stream close failed: %s",
                             esp_err_to_name(err));
                }
                xEventGroupSetBits(s_events, BT_EVENT_WORKER_QUIESCENT);
                continue;
            }

            active_command = command;
            channels = requested_channels;
            atomic_store_explicit(&s_audio_token, token, memory_order_release);
            atomic_store_explicit(&s_active_epoch, command >> 1,
                                  memory_order_release);
            atomic_store_explicit(&s_active_channels, channels,
                                  memory_order_release);
            atomic_store_explicit(&s_cancel_gate, 0, memory_order_release);
            atomic_store_explicit(&s_accepting_pcm, 1, memory_order_release);
            stream_open = true;

            if (atomic_load_explicit(&s_stream_command,
                                     memory_order_acquire) != active_command)
            {
                atomic_store_explicit(&s_accepting_pcm, 0,
                                      memory_order_release);
                cancel_active_audio();
                continue;
            }
        }

        if (stream_open)
        {
            uint32_t read_index = atomic_load_explicit(&s_ring_read,
                                                        memory_order_relaxed);
            uint32_t write_index = atomic_load_explicit(&s_ring_write,
                                                         memory_order_acquire);
            if (read_index != write_index)
            {
                pcm_slot_t *slot = &s_pcm_ring[read_index % BT_PCM_RING_SLOTS];
                uint32_t epoch = active_command >> 1;
                esp_err_t err = ESP_OK;
                if (slot->epoch == epoch && slot->len != 0
                    && slot->len % (sizeof(int16_t) * channels) == 0)
                {
                    size_t frames = slot->len / (sizeof(int16_t) * channels);
                    err = audio_pcm_stream_write(token, slot->samples, frames);
                }
                atomic_store_explicit(&s_ring_read, read_index + 1,
                                      memory_order_release);

                if (err != ESP_OK)
                {
                    uint32_t expected = active_command;
                    atomic_store_explicit(&s_accepting_pcm, 0,
                                          memory_order_release);
                    if (atomic_compare_exchange_strong_explicit(
                            &s_stream_command, &expected,
                            active_command & ~UINT32_C(1),
                            memory_order_acq_rel, memory_order_acquire))
                    {
                        ESP_LOGW(TAG, "audio PCM stream write stopped: %s",
                                 esp_err_to_name(err));
                    }
                }
                continue;
            }
        }
        else if ((command & 1) == 0)
        {
            ring_drop_all();
            xEventGroupSetBits(s_events, BT_EVENT_WORKER_QUIESCENT);
        }

        TickType_t wait = worker_wait_time(pending_drops, last_warning,
                                           has_warned);
        (void)ulTaskNotifyTake(pdTRUE, wait);
    }
}

static void note_pcm_overflow(void)
{
    atomic_fetch_add_explicit(&s_overflow_drops, 1, memory_order_relaxed);
    notify_worker();
}

static void a2dp_data_callback(const uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len == 0
        || atomic_load_explicit(&s_callbacks_active,
                                memory_order_acquire) == 0
        || atomic_load_explicit(&s_accepting_pcm,
                                memory_order_acquire) == 0)
    {
        return;
    }

    uint32_t channels = atomic_load_explicit(&s_active_channels,
                                              memory_order_acquire);
    if ((channels != 1 && channels != 2)
        || len % (sizeof(int16_t) * channels) != 0)
    {
        return;
    }
    if (len > sizeof(s_pcm_ring[0].samples))
    {
        note_pcm_overflow();
        return;
    }

    uint32_t write_index = atomic_load_explicit(&s_ring_write,
                                                 memory_order_relaxed);
    uint32_t read_index = atomic_load_explicit(&s_ring_read,
                                                memory_order_acquire);
    if (write_index - read_index >= BT_PCM_RING_SLOTS)
    {
        note_pcm_overflow();
        return;
    }

    uint32_t epoch = atomic_load_explicit(&s_active_epoch,
                                           memory_order_acquire);
    pcm_slot_t *slot = &s_pcm_ring[write_index % BT_PCM_RING_SLOTS];
    memcpy(slot->samples, buf, len);

    if (atomic_load_explicit(&s_accepting_pcm, memory_order_acquire) == 0
        || atomic_load_explicit(&s_active_epoch,
                                memory_order_acquire) != epoch)
    {
        return;
    }

    slot->epoch = epoch;
    slot->len = len;
    atomic_store_explicit(&s_ring_write, write_index + 1,
                          memory_order_release);
    notify_worker();
}

static bool parse_sbc_config(const esp_a2d_mcc_t *mcc, uint32_t *rate,
                             uint8_t *channels)
{
    if (mcc == NULL || mcc->type != ESP_A2D_MCT_SBC)
    {
        return false;
    }

    switch (mcc->cie.sbc_info.samp_freq)
    {
        case ESP_A2D_SBC_CIE_SF_16K:
            *rate = 16000;
            break;
        case ESP_A2D_SBC_CIE_SF_32K:
            *rate = 32000;
            break;
        case ESP_A2D_SBC_CIE_SF_44K:
            *rate = 44100;
            break;
        case ESP_A2D_SBC_CIE_SF_48K:
            *rate = 48000;
            break;
        default:
            return false;
    }

    switch (mcc->cie.sbc_info.ch_mode)
    {
        case ESP_A2D_SBC_CIE_CH_MODE_MONO:
            *channels = 1;
            break;
        case ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL:
        case ESP_A2D_SBC_CIE_CH_MODE_STEREO:
        case ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO:
            *channels = 2;
            break;
        default:
            return false;
    }
    return true;
}

static void copy_peer_address(esp_bd_addr_t destination)
{
    portENTER_CRITICAL(&s_peer_lock);
    memcpy(destination, s_peer_address, sizeof(esp_bd_addr_t));
    portEXIT_CRITICAL(&s_peer_lock);
}

static void a2dp_callback(esp_a2d_cb_event_t event,
                          esp_a2d_cb_param_t *param)
{
    if (param == NULL)
    {
        return;
    }

    if (event == ESP_A2D_PROF_STATE_EVT)
    {
        if (param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS)
        {
            atomic_store_explicit(&s_sink_ready, 1, memory_order_release);
            xEventGroupSetBits(s_events, BT_EVENT_A2DP_INIT);
        }
        else if (param->a2d_prof_stat.init_state == ESP_A2D_DEINIT_SUCCESS)
        {
            atomic_store_explicit(&s_sink_ready, 0, memory_order_release);
            atomic_store_explicit(&s_sink_requested, 0,
                                  memory_order_release);
            xEventGroupSetBits(s_events, BT_EVENT_A2DP_DEINIT);
        }
        return;
    }

    if (atomic_load_explicit(&s_callbacks_active,
                             memory_order_acquire) == 0)
    {
        return;
    }

    switch (event)
    {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED)
            {
                portENTER_CRITICAL(&s_peer_lock);
                memcpy(s_peer_address, param->conn_stat.remote_bda,
                       sizeof(esp_bd_addr_t));
                portEXIT_CRITICAL(&s_peer_lock);
                atomic_store_explicit(&s_connected, 1, memory_order_release);
                xEventGroupClearBits(s_events, BT_EVENT_DISCONNECTED);
                if (atomic_load_explicit(&s_shutting_down,
                                         memory_order_acquire) != 0)
                {
                    request_stream_stop();
                    (void)esp_a2d_sink_disconnect(param->conn_stat.remote_bda);
                }
            }
            else if (param->conn_stat.state
                     == ESP_A2D_CONNECTION_STATE_DISCONNECTED)
            {
                request_stream_stop();
                atomic_store_explicit(&s_stream_config, 0,
                                      memory_order_release);
                atomic_store_explicit(&s_connected, 0, memory_order_release);
                xEventGroupSetBits(s_events, BT_EVENT_DISCONNECTED);
            }
            else if (param->conn_stat.state
                     == ESP_A2D_CONNECTION_STATE_DISCONNECTING)
            {
                request_stream_stop();
            }
            break;

        case ESP_A2D_AUDIO_STATE_EVT:
            if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED
                && atomic_load_explicit(&s_shutting_down,
                                        memory_order_acquire) == 0)
            {
                request_stream_start();
            }
            else
            {
                request_stream_stop();
            }
            break;

        case ESP_A2D_AUDIO_CFG_EVT:
        {
            uint32_t rate;
            uint8_t channels;
            if (parse_sbc_config(&param->audio_cfg.mcc, &rate, &channels))
            {
                atomic_store_explicit(&s_stream_config,
                                      (rate << 8) | channels,
                                      memory_order_release);
                ESP_LOGI(TAG, "SBC stream configured: %lu Hz, %u channel(s)",
                         (unsigned long)rate, (unsigned)channels);
            }
            else
            {
                atomic_store_explicit(&s_stream_config, 0,
                                      memory_order_release);
                request_stream_stop();
                ESP_LOGW(TAG, "rejected invalid or non-SBC A2DP configuration");
            }
            break;
        }

        default:
            break;
    }
}

static void gap_callback(esp_bt_gap_cb_event_t event,
                         esp_bt_gap_cb_param_t *param)
{
    if (param == NULL
        || atomic_load_explicit(&s_callbacks_active,
                                memory_order_acquire) == 0)
    {
        return;
    }

    static esp_bt_pin_code_t pin = {'1', '2', '3', '4'};
    switch (event)
    {
        case ESP_BT_GAP_PIN_REQ_EVT:
            (void)esp_bt_gap_pin_reply(param->pin_req.bda,
                                       !param->pin_req.min_16_digit, 4, pin);
            break;
        case ESP_BT_GAP_CFM_REQ_EVT:
            (void)esp_bt_gap_ssp_confirm_reply(
                param->cfm_req.bda,
                atomic_load_explicit(&s_shutting_down,
                                     memory_order_acquire) == 0);
            break;
        case ESP_BT_GAP_KEY_REQ_EVT:
            (void)esp_bt_gap_ssp_passkey_reply(param->key_req.bda, false, 0);
            break;
        default:
            break;
    }
}

static void remember_error(esp_err_t *first_error, esp_err_t error,
                           const char *operation)
{
    if (error == ESP_OK)
    {
        return;
    }
    ESP_LOGW(TAG, "%s failed: %s", operation, esp_err_to_name(error));
    if (*first_error == ESP_OK)
    {
        *first_error = error;
    }
}

static bool resources_remain(void)
{
    return s_controller_initialized || s_controller_enabled
           || s_bluedroid_initialized || s_bluedroid_enabled
           || atomic_load_explicit(&s_sink_requested,
                                   memory_order_acquire) != 0;
}

static esp_err_t shutdown_stack(void)
{
    esp_err_t first_error = ESP_OK;
    atomic_store_explicit(&s_shutting_down, 1, memory_order_release);

    if (s_bluedroid_enabled)
    {
        remember_error(&first_error,
                       esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE,
                                                ESP_BT_NON_DISCOVERABLE),
                       "disable Bluetooth scan mode");
    }

    xEventGroupClearBits(s_events, BT_EVENT_WORKER_QUIESCENT);
    request_stream_stop();
    EventBits_t bits = xEventGroupWaitBits(
        s_events, BT_EVENT_WORKER_QUIESCENT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(BT_WORKER_QUIESCE_TIMEOUT_MS));
    if ((bits & BT_EVENT_WORKER_QUIESCENT) == 0)
    {
        remember_error(&first_error, ESP_ERR_TIMEOUT,
                       "quiesce Bluetooth PCM worker");
    }

    if (s_bluedroid_enabled)
    {
        xEventGroupClearBits(s_events, BT_EVENT_DISCONNECTED);
        if (atomic_load_explicit(&s_connected, memory_order_acquire) != 0)
        {
            esp_bd_addr_t peer;
            copy_peer_address(peer);
            esp_err_t err = esp_a2d_sink_disconnect(peer);
            remember_error(&first_error, err, "disconnect A2DP peer");
            if (err == ESP_OK)
            {
                bits = xEventGroupWaitBits(
                    s_events, BT_EVENT_DISCONNECTED, pdFALSE, pdTRUE,
                    pdMS_TO_TICKS(BT_DISCONNECT_TIMEOUT_MS));
                if ((bits & BT_EVENT_DISCONNECTED) == 0)
                {
                    remember_error(&first_error, ESP_ERR_TIMEOUT,
                                   "wait for A2DP disconnect");
                }
            }
        }
    }

    if (s_bluedroid_enabled
        && atomic_load_explicit(&s_sink_requested,
                                memory_order_acquire) != 0)
    {
        xEventGroupClearBits(s_events, BT_EVENT_A2DP_DEINIT);
        esp_err_t err = esp_a2d_sink_deinit();
        remember_error(&first_error, err, "request A2DP sink deinit");
        if (err == ESP_OK)
        {
            bits = xEventGroupWaitBits(
                s_events, BT_EVENT_A2DP_DEINIT, pdFALSE, pdTRUE,
                pdMS_TO_TICKS(BT_PROFILE_TIMEOUT_MS));
            if ((bits & BT_EVENT_A2DP_DEINIT) == 0)
            {
                remember_error(&first_error, ESP_ERR_TIMEOUT,
                               "wait for A2DP sink deinit");
            }
        }
    }

    atomic_store_explicit(&s_callbacks_active, 0, memory_order_release);
    atomic_store_explicit(&s_connected, 0, memory_order_release);
    atomic_store_explicit(&s_stream_config, 0, memory_order_release);

    if (s_bluedroid_enabled)
    {
        esp_err_t err = esp_bluedroid_disable();
        remember_error(&first_error, err, "disable Bluedroid");
        if (err == ESP_OK)
        {
            s_bluedroid_enabled = false;
        }
    }
    if (s_bluedroid_initialized && !s_bluedroid_enabled)
    {
        esp_err_t err = esp_bluedroid_deinit();
        remember_error(&first_error, err, "deinit Bluedroid");
        if (err == ESP_OK)
        {
            s_bluedroid_initialized = false;
            atomic_store_explicit(&s_sink_requested, 0,
                                  memory_order_release);
            atomic_store_explicit(&s_sink_ready, 0, memory_order_release);
        }
    }
    if (s_controller_enabled && !s_bluedroid_initialized)
    {
        esp_err_t err = esp_bt_controller_disable();
        remember_error(&first_error, err, "disable Bluetooth controller");
        if (err == ESP_OK)
        {
            s_controller_enabled = false;
        }
    }
    if (s_controller_initialized && !s_controller_enabled
        && !s_bluedroid_initialized)
    {
        esp_err_t err = esp_bt_controller_deinit();
        remember_error(&first_error, err, "deinit Bluetooth controller");
        if (err == ESP_OK)
        {
            s_controller_initialized = false;
        }
    }

    return first_error;
}

esp_err_t bluetooth_init(void)
{
    unsigned expected = BT_LIFECYCLE_UNINITIALIZED;
    if (!atomic_compare_exchange_strong_explicit(
            &s_lifecycle, &expected, BT_LIFECYCLE_INITIALIZING,
            memory_order_acq_rel, memory_order_acquire))
    {
        return expected == BT_LIFECYCLE_RUNNING ? ESP_OK
                                                : ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ensure_runtime();
    if (err != ESP_OK)
    {
        atomic_store_explicit(&s_lifecycle, BT_LIFECYCLE_UNINITIALIZED,
                              memory_order_release);
        return err;
    }

    xEventGroupClearBits(s_events, BT_EVENT_A2DP_INIT | BT_EVENT_A2DP_DEINIT
                                   | BT_EVENT_DISCONNECTED);
    atomic_store_explicit(&s_callbacks_active, 0, memory_order_release);
    atomic_store_explicit(&s_shutting_down, 0, memory_order_release);
    atomic_store_explicit(&s_connected, 0, memory_order_release);
    atomic_store_explicit(&s_sink_requested, 0, memory_order_release);
    atomic_store_explicit(&s_sink_ready, 0, memory_order_release);
    atomic_store_explicit(&s_stream_config, 0, memory_order_release);
    request_stream_stop();

    if (!s_ble_memory_released)
    {
        err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
        if (err != ESP_OK)
        {
            goto fail;
        }
        s_ble_memory_released = true;
    }

    esp_bt_controller_config_t controller_config =
        BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    controller_config.mode = ESP_BT_MODE_CLASSIC_BT;
    err = esp_bt_controller_init(&controller_config);
    if (err != ESP_OK)
    {
        goto fail;
    }
    s_controller_initialized = true;

    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK)
    {
        goto fail;
    }
    s_controller_enabled = true;

    esp_bluedroid_config_t bluedroid_config =
        BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bluedroid_config.ssp_en = true;
    err = esp_bluedroid_init_with_cfg(&bluedroid_config);
    if (err != ESP_OK)
    {
        goto fail;
    }
    s_bluedroid_initialized = true;

    err = esp_bluedroid_enable();
    if (err != ESP_OK)
    {
        goto fail;
    }
    s_bluedroid_enabled = true;
    atomic_store_explicit(&s_callbacks_active, 1, memory_order_release);

    err = esp_bt_gap_register_callback(gap_callback);
    if (err != ESP_OK)
    {
        goto fail;
    }
    err = esp_a2d_register_callback(a2dp_callback);
    if (err != ESP_OK)
    {
        goto fail;
    }

    err = esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE,
                                   ESP_BT_NON_DISCOVERABLE);
    if (err != ESP_OK)
    {
        goto fail;
    }

    esp_bt_io_cap_t io_capability = ESP_BT_IO_CAP_NONE;
    err = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE,
                                        &io_capability,
                                        sizeof(io_capability));
    if (err != ESP_OK)
    {
        goto fail;
    }
    esp_bt_pin_code_t pin = {'1', '2', '3', '4'};
    err = esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin);
    if (err != ESP_OK)
    {
        goto fail;
    }
    err = esp_bt_gap_set_device_name(BLUETOOTH_DEVICE_NAME);
    if (err != ESP_OK)
    {
        goto fail;
    }

    err = esp_a2d_sink_init();
    if (err != ESP_OK)
    {
        goto fail;
    }
    atomic_store_explicit(&s_sink_requested, 1, memory_order_release);

    err = esp_a2d_sink_register_data_callback(a2dp_data_callback);
    if (err != ESP_OK)
    {
        goto fail;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_events, BT_EVENT_A2DP_INIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(BT_PROFILE_TIMEOUT_MS));
    if ((bits & BT_EVENT_A2DP_INIT) == 0
        || atomic_load_explicit(&s_sink_ready, memory_order_acquire) == 0)
    {
        err = ESP_ERR_TIMEOUT;
        goto fail;
    }

    err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                   ESP_BT_GENERAL_DISCOVERABLE);
    if (err != ESP_OK)
    {
        goto fail;
    }

    atomic_store_explicit(&s_lifecycle, BT_LIFECYCLE_RUNNING,
                          memory_order_release);
    ESP_LOGI(TAG, "%s is connectable and discoverable", BLUETOOTH_DEVICE_NAME);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Bluetooth initialization failed: %s", esp_err_to_name(err));
    (void)shutdown_stack();
    atomic_store_explicit(&s_lifecycle,
                          resources_remain()
                              ? BT_LIFECYCLE_CLEANUP_REQUIRED
                              : BT_LIFECYCLE_UNINITIALIZED,
                          memory_order_release);
    return err;
}

esp_err_t bluetooth_stop(void)
{
    unsigned state = atomic_load_explicit(&s_lifecycle, memory_order_acquire);
    for (;;)
    {
        if (state == BT_LIFECYCLE_UNINITIALIZED)
        {
            return ESP_OK;
        }
        if (state == BT_LIFECYCLE_INITIALIZING
            || state == BT_LIFECYCLE_STOPPING)
        {
            return ESP_ERR_INVALID_STATE;
        }
        if (atomic_compare_exchange_weak_explicit(
                &s_lifecycle, &state, BT_LIFECYCLE_STOPPING,
                memory_order_acq_rel, memory_order_acquire))
        {
            break;
        }
    }

    esp_err_t err = shutdown_stack();
    atomic_store_explicit(&s_lifecycle,
                          resources_remain()
                              ? BT_LIFECYCLE_CLEANUP_REQUIRED
                              : BT_LIFECYCLE_UNINITIALIZED,
                          memory_order_release);
    return err;
}

bool bluetooth_is_connected(void)
{
    return atomic_load_explicit(&s_connected, memory_order_acquire) != 0;
}
