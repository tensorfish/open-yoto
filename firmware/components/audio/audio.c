/*
 * audio.c — Yoto Player MP3, ADTS AAC, and M4A/AAC-LC playback.
 *
 * The stock firmware's ADF pipeline is reader -> decoder -> resampler ->
 * 44.1 kHz mono I2S. This replacement keeps that contract with bounded
 * streaming parsers, Espressif MP3/AAC decoders, source-rate/channel
 * propagation, stereo downmix, and a stateful resampler feeding the AW88194A
 * and ES8156.
 */
#include "audio.h"

#include "aw88194.h"
#include "board_pins.h"
#include "codec_es8156.h"
#include "iox.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_aac_dec.h"
#include "esp_mp3_dec.h"
#include "esp_audio_dec.h"
#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "audio";

/* Task notification bits (a bitmask in the task's notification value). */
#define AUDIO_NOTIFY_STOP   (1u << 0)
#define AUDIO_NOTIFY_PLAY   (1u << 1)

/* Bounded encoded-data window, large enough for maximum Layer III frames and
 * AAC access units accepted by the Espressif decoders. */
#define AUDIO_INPUT_BUF_SIZE    4096

/* Maximum PCM samples per decoded frame: MPEG1 layer 3 stereo = 1152 samples
 * per channel = 2304 interleaved samples. */
#define AUDIO_PCM_BUF_SAMPLES   2304
#define AUDIO_RESAMPLE_BUF_SAMPLES 512

/* I2S DMA write timeout. Kept short so the decode task stays responsive to
 * stop/pause while still providing natural backpressure on a full DMA. */
#define AUDIO_I2S_WRITE_TICK_MS 20
#define AUDIO_I2S_SILENCE_SAMPLES 256
#define AUDIO_HP_POLL_MS          100
/* The jack switch pulls hpdetect low when a plug is present. */
#define AUDIO_HPDETECT_ACTIVE_LOW 1

/* Decode task stack size in bytes (ESP-IDF FreeRTOS convention). */
#define AUDIO_DECODE_STACK_BYTES 20480

/* Decode task priority. Below the encoder (5) but above idle (0). */
#define AUDIO_DECODE_PRIORITY   4

/* The boot speaker test runs independently so encoder events remain live. */
#define AUDIO_TONE_STACK_BYTES      3072
#define AUDIO_TONE_PRIORITY         4
#define AUDIO_TONE_AMPLITUDE       16000

/* A volume blip is feedback, not playback: quieter than the test tone, short
 * enough to feel like a click, and ramped so the square wave cannot crack the
 * speaker. */
#define AUDIO_BLIP_AMPLITUDE        9000
#define AUDIO_BLIP_MAX_MS            250
#define AUDIO_BLIP_FADE_MS             4

/* Path handed from audio_play() to the decode task. */
#define AUDIO_PATH_MAX          128

/* TX channel handle for the i2s_std driver. */
static i2s_chan_handle_t s_tx_chan = NULL;

/* Decode task handle, created in audio_init(). */
static TaskHandle_t s_decode_task = NULL;

/* Optional tone task used by the speaker smoke test. */
static TaskHandle_t s_tone_task = NULL;

/* Shared playback state. s_stop_req and s_paused are written from the calling
 * thread and read (polled) by the decode task; s_playing is written by the
 * decode task and read by audio_is_playing(). Volatile makes those cross-task
 * accesses well-defined. */
static volatile bool s_playing = false;
static volatile bool s_stop_req = false;
static volatile bool s_paused = false;

/* Volume is applied as PCM gain so it affects both the ES8156 and the
 * shared-I2S AW881xx speaker path. The fixed ES8156 hardware gain remains its
 * safe initialization default. */
static volatile int s_volume = 70;
static int16_t s_scaled_pcm[AUDIO_PCM_BUF_SAMPLES];
static int16_t s_resampled_pcm[AUDIO_RESAMPLE_BUF_SAMPLES];
static unsigned char s_encoded_input[AUDIO_INPUT_BUF_SIZE];

/* File path for the current playback request, installed before the PLAY
 * notification is issued to the decode task. */
static char s_path[AUDIO_PATH_MAX];

/* Headphone routing is sampled by the decode task while I2S is active. */
static TickType_t s_headphone_poll_ticks;
static bool s_headphone_state_valid;
static bool s_headphone_inserted;

/**
 * Configure the I2S std TX channel exactly as the stock playback pipeline
 * settles it: master, APLL 44100 Hz, 16-bit Philips/I2S, mono-left.
 *
 * return ESP_OK on success, otherwise the first I2S driver error.
 */
static esp_err_t audio_i2s_init(void)
{
    esp_err_t err;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT,
                                                            I2S_ROLE_MASTER);
    i2s_std_clk_config_t clk_cfg =
        I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE_HZ);
    i2s_std_slot_config_t slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                            I2S_SLOT_MODE_MONO);

    /* Clear each descriptor after transmission so an underrun emits silence
     * instead of replaying the previous PCM fragment. */
    chan_cfg.auto_clear_after_cb = true;

    /* Stock i2s_stream config @ 0x401d8d16 uses APLL, then
     * i2s_set_clk(44100, 16, 1) @ 0x401d8d82 switches to mono-left. On ESP32
     * legacy I2S that transition also inverts WS polarity. */
    clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    slot_cfg.ws_pol = true;
    err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    /* MCLK = 256 * fs, 16-bit mono-left Philips. */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = clk_cfg,
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_LRCLK,
            .dout = PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "I2S TX enabled (APLL, mono-left, mclk=%d bclk=%d lrclk=%d "
             "dout=%d, %d Hz)",
             PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT,
             I2S_SAMPLE_RATE_HZ);
    return ESP_OK;
}

/*
 * Stop the cyclic TX DMA, overwrite every descriptor with silence, and resume
 * clocks. Without this reset, an idle ESP32 I2S TX ring repeats its final PCM
 * fragment after the decoder stops producing buffers.
 */
static esp_err_t audio_i2s_clear(void)
{
    static const int16_t silence[AUDIO_I2S_SILENCE_SAMPLES] = { 0 };
    esp_err_t err = i2s_channel_disable(s_tx_chan);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_channel_disable failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    for (int i = 0; i < 8; i++)
    {
        size_t loaded = 0;

        err = i2s_channel_preload_data(s_tx_chan, silence, sizeof(silence),
                                       &loaded);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "I2S silence preload failed: %s",
                     esp_err_to_name(err));
            break;
        }
        if (loaded < sizeof(silence))
        {
            break;
        }
    }
    esp_err_t enable_err = i2s_channel_enable(s_tx_chan);
    if (enable_err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_channel_enable after clear failed: %s",
                 esp_err_to_name(enable_err));
        return enable_err;
    }
    return err;
}

/* Mute only the SmartPA when a headphone plug is inserted; I2S continues to
 * feed the ES8156 headphone DAC. A transient IOX read failure leaves the
 * current speaker state untouched. */
static void audio_update_headphone_route(bool force)
{
    TickType_t now = xTaskGetTickCount();
    uint8_t port;
    bool level;
    bool inserted;
    esp_err_t err;

    if (!force && s_headphone_state_valid
        && now - s_headphone_poll_ticks < pdMS_TO_TICKS(AUDIO_HP_POLL_MS))
    {
        return;
    }
    s_headphone_poll_ticks = now;
    if (iox_read_port(IOX_EXP(IOX_AUDIO_HPDETECT),
                      IOX_PORT(IOX_AUDIO_HPDETECT), &port) != ESP_OK)
    {
        ESP_LOGW(TAG, "headphone detect read failed");
        return;
    }
    level = (port & (1u << IOX_BIT(IOX_AUDIO_HPDETECT))) != 0;
#if AUDIO_HPDETECT_ACTIVE_LOW
    inserted = !level;
#else
    inserted = level;
#endif
    if (s_headphone_state_valid && inserted == s_headphone_inserted)
    {
        return;
    }

    err = aw88194_set_muted(inserted);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "speaker %s failed: %s",
                 inserted ? "mute" : "unmute", esp_err_to_name(err));
        return;
    }
    s_headphone_inserted = inserted;
    s_headphone_state_valid = true;
    ESP_LOGI(TAG, "headphones %s: speaker %s",
             inserted ? "inserted" : "removed",
             inserted ? "muted" : "enabled");
}

/**
 * Skip an ID3v2 header at the front of an MP3 stream.
 *
 * fp file positioned at the start of the file. On return it is positioned at
 *     the first MP3 frame byte (after any ID3v2 tag).
 *
 * return ESP_OK on success (including when there is no ID3v2 tag), otherwise
 *        ESP_FAIL if the file cannot be read or positioned.
 */
static esp_err_t audio_skip_id3v2(FILE *fp)
{
    unsigned char hdr[10];

    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr))
    {
        return ESP_FAIL;
    }

    /* ID3v2 tag = "ID3" + version + revision + flags + 4-byte synchsafe size,
     * where each size byte contributes only its low 7 bits. */
    if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3')
    {
        unsigned long tag_size = ((unsigned long)(hdr[6] & 0x7F) << 21) |
                                 ((unsigned long)(hdr[7] & 0x7F) << 14) |
                                 ((unsigned long)(hdr[8] & 0x7F) << 7)  |
                                 ((unsigned long)(hdr[9] & 0x7F));
        if (fseek(fp, (long)tag_size, SEEK_CUR) != 0)
        {
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "skipped %lu-byte ID3v2 tag", tag_size);
    }
    else
    {
        /* Not an ID3v2 tag: rewind so the header bytes are decoded as data. */
        if (fseek(fp, 0, SEEK_SET) != 0)
        {
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

/**
 * Refill the decoder input window. Leftover bytes are compacted to the front
 * of the buffer and new data is read from the file after them.
 *
 * fp     open MP3 file, positioned just past the data already consumed.
 * buf    input window buffer.
 * in_ptr input/output: current read pointer within buf, advanced to buf[0].
 * left   input/output: valid byte count in buf, updated with the new data.
 *
 * return 0 when new data was read, -1 at end of file (no bytes remain).
 */
static int audio_fill(FILE *fp, unsigned char *buf,
                      const unsigned char **in_ptr, size_t *left)
{
    size_t space;
    size_t n;

    /* Keep any undecoded tail, freeing the rest of the buffer for new data. */
    if (*left > 0 && *in_ptr != buf)
    {
        memmove(buf, *in_ptr, *left);
    }
    *in_ptr = buf;

    space = AUDIO_INPUT_BUF_SIZE - *left;
    n = fread(buf + *left, 1, space, fp);
    if (n == 0)
    {
        return -1;
    }
    *left += n;
    return 0;
}

/**
 * Write mono 16-bit PCM to I2S, honoring pause/stop between writes.
 *
 * @param[in] pcm Mono 16-bit PCM samples.
 * @param[in] samples Number of samples in pcm.
 *
 * PCM is scaled in bounded chunks using the current volume. This makes volume
 * affect every device sharing I2S, including the speaker amp.
 *
 * @return True if all samples were written; false if playback stopped.
 */
static bool audio_write_pcm(const int16_t *pcm, size_t samples)
{
    size_t offset = 0;

    while (offset < samples)
    {
        audio_update_headphone_route(false);
        const int16_t *output;
        size_t chunk_samples = samples - offset;
        size_t chunk_offset = 0;
        int volume = s_volume;

        if (chunk_samples > AUDIO_PCM_BUF_SAMPLES)
        {
            chunk_samples = AUDIO_PCM_BUF_SAMPLES;
        }
        if (volume < 0)
        {
            volume = 0;
        }
        else if (volume > 100)
        {
            volume = 100;
        }

        output = pcm + offset;
        if (volume < 100)
        {
            for (size_t i = 0; i < chunk_samples; i++)
            {
                s_scaled_pcm[i] = (int16_t)(((int32_t)output[i] * volume) / 100);
            }
            output = s_scaled_pcm;
        }

        while (chunk_offset < chunk_samples)
        {
            size_t written = 0;
            esp_err_t err;

            if (s_stop_req)
            {
                return false;
            }
            while (s_paused && !s_stop_req)
            {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (s_stop_req)
            {
                return false;
            }

            err = i2s_channel_write(s_tx_chan, output + chunk_offset,
                                    (chunk_samples - chunk_offset) * sizeof(int16_t),
                                    &written,
                                    pdMS_TO_TICKS(AUDIO_I2S_WRITE_TICK_MS));
            if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
            {
                ESP_LOGE(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err));
                return false;
            }
            chunk_offset += written / sizeof(int16_t);
        }
        offset += chunk_samples;
    }
    return true;
}

typedef struct
{
    uint32_t source_rate;
    uint64_t input_index;
    uint64_t next_output_index;
    int16_t previous;
    bool have_previous;
} audio_resampler_t;

/*
 * Linearly resample a continuous mono stream to the stock 44.1 kHz I2S clock.
 * State spans MP3 frame boundaries so neither samples nor fractional phase are
 * lost between calls.
 */
static bool audio_write_resampled(audio_resampler_t *state,
                                  const int16_t *pcm, size_t samples,
                                  uint32_t source_rate)
{
    size_t produced = 0;
    size_t start = 0;

    if (source_rate == I2S_SAMPLE_RATE_HZ)
    {
        return audio_write_pcm(pcm, samples);
    }
    if (source_rate < 8000 || source_rate > 96000 || samples == 0)
    {
        ESP_LOGE(TAG, "unsupported source sample rate: %lu Hz",
                 (unsigned long)source_rate);
        return false;
    }
    if (state->source_rate != source_rate)
    {
        memset(state, 0, sizeof(*state));
        state->source_rate = source_rate;
        ESP_LOGI(TAG, "resampling source from %lu Hz to %d Hz",
                 (unsigned long)source_rate, I2S_SAMPLE_RATE_HZ);
    }
    if (!state->have_previous)
    {
        state->previous = pcm[0];
        state->have_previous = true;
        state->next_output_index = 1;
        s_resampled_pcm[produced++] = pcm[0];
        start = 1;
    }

    for (size_t i = start; i < samples; i++)
    {
        uint64_t next_input = state->input_index + 1;
        uint64_t interval_end = next_input * I2S_SAMPLE_RATE_HZ;

        while (state->next_output_index * source_rate <= interval_end)
        {
            uint64_t position = state->next_output_index * source_rate;
            uint32_t fraction = (uint32_t)(
                position - state->input_index * I2S_SAMPLE_RATE_HZ);
            int32_t delta = (int32_t)pcm[i] - state->previous;
            int32_t sample = state->previous
                           + (int32_t)(((int64_t)delta * fraction)
                                       / I2S_SAMPLE_RATE_HZ);

            s_resampled_pcm[produced++] = (int16_t)sample;
            state->next_output_index++;
            if (produced == AUDIO_RESAMPLE_BUF_SAMPLES)
            {
                if (!audio_write_pcm(s_resampled_pcm, produced))
                {
                    return false;
                }
                produced = 0;
            }
        }
        state->previous = pcm[i];
        state->input_index = next_input;
    }

    return produced == 0 || audio_write_pcm(s_resampled_pcm, produced);
}

static bool audio_write_decoded(audio_resampler_t *resampler, int16_t *pcm,
                                size_t output_samples, int channels,
                                uint32_t sample_rate)
{
    if (channels == 2)
    {
        size_t frames = output_samples / 2;

        for (size_t i = 0; i < frames; i++)
        {
            int32_t mixed = (int32_t)pcm[i * 2]
                          + (int32_t)pcm[i * 2 + 1];
            pcm[i] = (int16_t)(mixed / 2);
        }
        output_samples = frames;
    }
    else if (channels != 1)
    {
        ESP_LOGE(TAG, "unsupported channel count: %d", channels);
        return false;
    }
    return audio_write_resampled(resampler, pcm, output_samples, sample_rate);
}

static bool audio_write_esp_frame(audio_resampler_t *resampler, int16_t *pcm,
                                  size_t pcm_capacity,
                                  const esp_audio_dec_out_frame_t *frame,
                                  const esp_audio_dec_info_t *info)
{
    if (info->bits_per_sample != 16
        || (frame->decoded_size & 1) != 0
        || frame->decoded_size / sizeof(int16_t) > pcm_capacity)
    {
        ESP_LOGE(TAG, "invalid decoded PCM format: %u-bit, %lu bytes",
                 (unsigned)info->bits_per_sample,
                 (unsigned long)frame->decoded_size);
        return false;
    }
    return audio_write_decoded(resampler, pcm,
                               frame->decoded_size / sizeof(int16_t),
                               info->channel, info->sample_rate);
}

typedef struct
{
    uint32_t data_offset;
    uint32_t data_size;
} mp4_box_t;

typedef struct
{
    mp4_box_t stsz;
    mp4_box_t stsc;
    mp4_box_t chunks;
    uint32_t file_size;
    uint32_t fixed_sample_size;
    uint32_t sample_count;
    uint32_t stsc_count;
    uint32_t chunk_count;
    uint32_t sample_rate;
    uint16_t channels;
    bool chunk_offsets_64;
} m4a_tables_t;

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t read_be64(const uint8_t *p)
{
    return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

static bool audio_read_at(FILE *fp, uint32_t offset, void *out, size_t len)
{
    return offset <= LONG_MAX
        && fseek(fp, (long)offset, SEEK_SET) == 0
        && fread(out, 1, len, fp) == len;
}

static bool mp4_is_container(const uint8_t type[4])
{
    return memcmp(type, "moov", 4) == 0 || memcmp(type, "trak", 4) == 0
        || memcmp(type, "mdia", 4) == 0 || memcmp(type, "minf", 4) == 0
        || memcmp(type, "stbl", 4) == 0;
}

static bool mp4_find_box(FILE *fp, uint32_t start, uint32_t length,
                         const char wanted[4], mp4_box_t *out, unsigned depth)
{
    uint32_t position = start;
    uint32_t end;

    if (depth > 8 || length > UINT32_MAX - start)
    {
        return false;
    }
    end = start + length;
    while (position <= end && end - position >= 8)
    {
        uint8_t header[16];
        uint8_t type[4];
        uint64_t box_size;
        uint32_t header_size = 8;
        uint32_t data_offset;
        uint32_t data_size;

        if (!audio_read_at(fp, position, header, 8))
        {
            return false;
        }
        memcpy(type, header + 4, sizeof(type));
        box_size = read_be32(header);
        if (box_size == 1)
        {
            if (!audio_read_at(fp, position + 8, header + 8, 8))
            {
                return false;
            }
            box_size = read_be64(header + 8);
            header_size = 16;
        }
        else if (box_size == 0)
        {
            box_size = end - position;
        }
        if (box_size < header_size || box_size > end - position
            || box_size > UINT32_MAX)
        {
            return false;
        }
        data_offset = position + header_size;
        data_size = (uint32_t)box_size - header_size;
        if (memcmp(type, wanted, 4) == 0)
        {
            out->data_offset = data_offset;
            out->data_size = data_size;
            return true;
        }
        if (mp4_is_container(type)
            && mp4_find_box(fp, data_offset, data_size, wanted, out, depth + 1))
        {
            return true;
        }
        position += (uint32_t)box_size;
    }
    return false;
}

static bool m4a_parse_tables(FILE *fp, m4a_tables_t *tables)
{
    mp4_box_t stsd;
    uint8_t stsd_data[44];
    uint8_t header[12];
    long file_size;

    memset(tables, 0, sizeof(*tables));
    if (fseek(fp, 0, SEEK_END) != 0
        || (file_size = ftell(fp)) <= 0
        || (unsigned long)file_size > UINT32_MAX)
    {
        return false;
    }
    tables->file_size = (uint32_t)file_size;
    if (!mp4_find_box(fp, 0, tables->file_size, "stsd", &stsd, 0)
        || !mp4_find_box(fp, 0, tables->file_size, "stsz", &tables->stsz, 0)
        || !mp4_find_box(fp, 0, tables->file_size, "stsc", &tables->stsc, 0))
    {
        return false;
    }
    if (!mp4_find_box(fp, 0, tables->file_size, "stco", &tables->chunks, 0))
    {
        if (!mp4_find_box(fp, 0, tables->file_size, "co64",
                          &tables->chunks, 0))
        {
            return false;
        }
        tables->chunk_offsets_64 = true;
    }
    if (stsd.data_size < sizeof(stsd_data)
        || !audio_read_at(fp, stsd.data_offset, stsd_data, sizeof(stsd_data)))
    {
        return false;
    }

    const uint8_t *entry = stsd_data + 8;
    uint32_t entry_size = read_be32(entry);
    if (read_be32(stsd_data + 4) == 0
        || entry_size < 36 || entry_size > stsd.data_size - 8
        || memcmp(entry + 4, "mp4a", 4) != 0)
    {
        return false;
    }
    tables->channels = read_be16(entry + 24);
    tables->sample_rate = read_be32(entry + 32) >> 16;
    if (tables->channels == 0 || tables->channels > 2
        || tables->sample_rate < 8000 || tables->sample_rate > 96000)
    {
        return false;
    }

    if (tables->stsz.data_size < sizeof(header)
        || !audio_read_at(fp, tables->stsz.data_offset, header, sizeof(header)))
    {
        return false;
    }
    tables->fixed_sample_size = read_be32(header + 4);
    tables->sample_count = read_be32(header + 8);
    if (tables->sample_count == 0
        || (tables->fixed_sample_size == 0
            && tables->stsz.data_size - 12 < tables->sample_count * 4ULL))
    {
        return false;
    }

    if (tables->stsc.data_size < 8
        || !audio_read_at(fp, tables->stsc.data_offset + 4, header, 4))
    {
        return false;
    }
    tables->stsc_count = read_be32(header);
    if (tables->stsc_count == 0
        || tables->stsc.data_size - 8 < tables->stsc_count * 12ULL)
    {
        return false;
    }

    if (tables->chunks.data_size < 8
        || !audio_read_at(fp, tables->chunks.data_offset + 4, header, 4))
    {
        return false;
    }
    tables->chunk_count = read_be32(header);
    size_t offset_bytes = tables->chunk_offsets_64 ? 8 : 4;
    return tables->chunk_count > 0
        && tables->chunks.data_size - 8
            >= (uint64_t)tables->chunk_count * offset_bytes;
}

static bool m4a_chunk_offset(FILE *fp, const m4a_tables_t *tables,
                             uint32_t chunk_index, uint32_t *offset)
{
    uint8_t data[8];
    size_t width = tables->chunk_offsets_64 ? 8 : 4;
    uint32_t position = tables->chunks.data_offset + 8
                      + (chunk_index - 1) * width;

    if (!audio_read_at(fp, position, data, width))
    {
        return false;
    }
    if (tables->chunk_offsets_64)
    {
        uint64_t value = read_be64(data);
        if (value > UINT32_MAX)
        {
            return false;
        }
        *offset = (uint32_t)value;
    }
    else
    {
        *offset = read_be32(data);
    }
    return *offset < tables->file_size;
}

static bool m4a_samples_per_chunk(FILE *fp, const m4a_tables_t *tables,
                                  uint32_t chunk_index, uint32_t *samples)
{
    uint8_t entry[12];
    uint32_t selected = 0;

    for (uint32_t i = 0; i < tables->stsc_count; i++)
    {
        if (!audio_read_at(fp, tables->stsc.data_offset + 8 + i * 12,
                           entry, sizeof(entry)))
        {
            return false;
        }
        if (read_be32(entry) > chunk_index)
        {
            break;
        }
        selected = read_be32(entry + 4);
    }
    *samples = selected;
    return selected > 0;
}

static esp_err_t audio_decode_m4a_aac(FILE *fp, const char *path,
                                      int16_t *pcm, size_t pcm_capacity)
{
    m4a_tables_t tables;
    esp_aac_dec_cfg_t config = ESP_AAC_DEC_CONFIG_DEFAULT();
    void *decoder = NULL;
    FILE *table_fp = NULL;
    audio_resampler_t resampler = { 0 };
    uint32_t sample_index = 0;
    esp_err_t result = ESP_FAIL;

    if (!m4a_parse_tables(fp, &tables))
    {
        ESP_LOGE(TAG, "invalid or unsupported M4A sample tables");
        return ESP_ERR_INVALID_RESPONSE;
    }
    table_fp = fopen(path, "rb");
    if (table_fp == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    config.sample_rate = (int32_t)tables.sample_rate;
    config.channel = (uint8_t)tables.channels;
    config.bits_per_sample = 16;
    config.no_adts_header = true;
    config.aac_plus_enable = false;
    if (esp_aac_dec_open(&config, sizeof(config), &decoder)
        != ESP_AUDIO_ERR_OK)
    {
        ESP_LOGE(TAG, "Espressif AAC decoder open failed");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    ESP_LOGI(TAG, "M4A AAC-LC: %lu samples, %u channel(s), %lu Hz",
             (unsigned long)tables.sample_count, (unsigned)tables.channels,
             (unsigned long)tables.sample_rate);

    for (uint32_t chunk = 1;
         chunk <= tables.chunk_count && sample_index < tables.sample_count
         && !s_stop_req;
         chunk++)
    {
        uint32_t chunk_offset;
        uint32_t samples_in_chunk;

        if (!m4a_chunk_offset(table_fp, &tables, chunk, &chunk_offset)
            || !m4a_samples_per_chunk(table_fp, &tables, chunk,
                                      &samples_in_chunk)
            || fseek(fp, (long)chunk_offset, SEEK_SET) != 0)
        {
            ESP_LOGE(TAG, "invalid M4A chunk %lu", (unsigned long)chunk);
            goto cleanup;
        }

        for (uint32_t in_chunk = 0;
             in_chunk < samples_in_chunk && sample_index < tables.sample_count;
             in_chunk++, sample_index++)
        {
            uint32_t sample_size = tables.fixed_sample_size;
            uint8_t encoded_size[4];
            esp_audio_dec_in_raw_t raw = { 0 };
            esp_audio_dec_out_frame_t frame = { 0 };
            esp_audio_dec_info_t info = { 0 };
            esp_audio_err_t decode_result;

            if (sample_size == 0)
            {
                if (!audio_read_at(
                        table_fp,
                        tables.stsz.data_offset + 12 + sample_index * 4,
                        encoded_size, sizeof(encoded_size)))
                {
                    goto cleanup;
                }
                sample_size = read_be32(encoded_size);
            }
            if (sample_size == 0 || sample_size > sizeof(s_encoded_input)
                || fread(s_encoded_input, 1, sample_size, fp) != sample_size)
            {
                ESP_LOGE(TAG, "invalid M4A AAC sample %lu size %lu",
                         (unsigned long)sample_index,
                         (unsigned long)sample_size);
                goto cleanup;
            }

            raw.buffer = s_encoded_input;
            raw.len = sample_size;
            frame.buffer = (uint8_t *)pcm;
            frame.len = pcm_capacity * sizeof(int16_t);
            decode_result = esp_aac_dec_decode(decoder, &raw, &frame, &info);
            if (decode_result != ESP_AUDIO_ERR_OK)
            {
                ESP_LOGE(TAG, "AAC decode sample %lu failed: %d",
                         (unsigned long)sample_index, (int)decode_result);
                goto cleanup;
            }
            if (!audio_write_esp_frame(&resampler, pcm, pcm_capacity,
                                       &frame, &info))
            {
                result = ESP_ERR_INVALID_STATE;
                goto cleanup;
            }
        }
    }

    result = s_stop_req ? ESP_ERR_INVALID_STATE
           : sample_index == tables.sample_count ? ESP_OK : ESP_FAIL;

cleanup:
    if (decoder != NULL)
    {
        (void)esp_aac_dec_close(decoder);
    }
    if (table_fp != NULL)
    {
        fclose(table_fp);
    }
    return result;
}

static esp_err_t audio_decode_adts_aac(FILE *fp, unsigned char *buffer,
                                       int16_t *pcm, size_t pcm_capacity)
{
    esp_aac_dec_cfg_t config = ESP_AAC_DEC_CONFIG_DEFAULT();
    void *decoder = NULL;
    const unsigned char *input = buffer;
    size_t left = 0;
    audio_resampler_t resampler = { 0 };
    bool decoded = false;
    esp_err_t result = ESP_FAIL;

    config.no_adts_header = false;
    config.aac_plus_enable = false;
    if (esp_aac_dec_open(&config, sizeof(config), &decoder)
        != ESP_AUDIO_ERR_OK)
    {
        return ESP_ERR_NO_MEM;
    }
    while (!s_stop_req)
    {
        size_t sync = 0;
        size_t frame_size;
        esp_audio_dec_in_raw_t raw = { 0 };
        esp_audio_dec_out_frame_t frame = { 0 };
        esp_audio_dec_info_t info = { 0 };
        esp_audio_err_t decode_result;

        if (left < 7 && audio_fill(fp, buffer, &input, &left) != 0)
        {
            break;
        }
        while (sync + 1 < left
               && !(input[sync] == 0xFF
                    && (input[sync + 1] & 0xF6) == 0xF0))
        {
            sync++;
        }
        if (sync + 1 >= left)
        {
            if (left > 1)
            {
                input += left - 1;
                left = 1;
            }
            if (audio_fill(fp, buffer, &input, &left) != 0)
            {
                break;
            }
            continue;
        }
        input += sync;
        left -= sync;
        if (left < 7 && audio_fill(fp, buffer, &input, &left) != 0)
        {
            break;
        }
        frame_size = ((size_t)(input[3] & 0x03) << 11)
                   | ((size_t)input[4] << 3) | (input[5] >> 5);
        if (frame_size < 7 || frame_size > AUDIO_INPUT_BUF_SIZE)
        {
            input++;
            left--;
            continue;
        }
        if (left < frame_size
            && (audio_fill(fp, buffer, &input, &left) != 0
                || left < frame_size))
        {
            break;
        }

        raw.buffer = (uint8_t *)input;
        raw.len = frame_size;
        frame.buffer = (uint8_t *)pcm;
        frame.len = pcm_capacity * sizeof(int16_t);
        decode_result = esp_aac_dec_decode(decoder, &raw, &frame, &info);
        input += frame_size;
        left -= frame_size;
        if (decode_result != ESP_AUDIO_ERR_OK)
        {
            ESP_LOGW(TAG, "ADTS AAC frame decode failed: %d",
                     (int)decode_result);
            continue;
        }
        if (!audio_write_esp_frame(&resampler, pcm, pcm_capacity,
                                   &frame, &info))
        {
            result = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
        decoded = true;
    }
    result = s_stop_req ? ESP_ERR_INVALID_STATE
           : decoded ? ESP_OK : ESP_ERR_INVALID_RESPONSE;

cleanup:
    (void)esp_aac_dec_close(decoder);
    return result;
}

typedef enum
{
    AUDIO_FORMAT_MP3,
    AUDIO_FORMAT_M4A,
    AUDIO_FORMAT_AAC_ADTS,
    AUDIO_FORMAT_UNKNOWN,
} audio_format_t;

static bool mp3_frame_size(const unsigned char header[4], size_t *frame_size)
{
    static const uint16_t bitrate_mpeg1_l3[16] = {
        0, 32, 40, 48, 56, 64, 80, 96,
        112, 128, 160, 192, 224, 256, 320, 0
    };
    static const uint16_t bitrate_mpeg2_l3[16] = {
        0, 8, 16, 24, 32, 40, 48, 56,
        64, 80, 96, 112, 128, 144, 160, 0
    };
    static const uint32_t sample_rates[3] = { 44100, 48000, 32000 };
    uint32_t value = ((uint32_t)header[0] << 24)
                   | ((uint32_t)header[1] << 16)
                   | ((uint32_t)header[2] << 8) | header[3];
    uint8_t version = (uint8_t)((value >> 19) & 0x03);
    uint8_t layer = (uint8_t)((value >> 17) & 0x03);
    uint8_t bitrate_index = (uint8_t)((value >> 12) & 0x0F);
    uint8_t rate_index = (uint8_t)((value >> 10) & 0x03);
    uint8_t padding = (uint8_t)((value >> 9) & 0x01);
    uint32_t sample_rate;
    uint32_t bitrate;

    if ((value & 0xFFE00000) != 0xFFE00000
        || version == 1 || layer != 1 || rate_index == 3
        || bitrate_index == 0 || bitrate_index == 15)
    {
        return false;
    }
    sample_rate = sample_rates[rate_index];
    if (version == 2)
    {
        sample_rate /= 2;
    }
    else if (version == 0)
    {
        sample_rate /= 4;
    }
    bitrate = (version == 3 ? bitrate_mpeg1_l3[bitrate_index]
                            : bitrate_mpeg2_l3[bitrate_index]) * 1000;
    *frame_size = (version == 3 ? 144 : 72) * bitrate / sample_rate + padding;
    return *frame_size >= 4 && *frame_size <= AUDIO_INPUT_BUF_SIZE;
}

static int mp3_find_frame(const unsigned char *data, size_t len,
                          size_t *frame_size)
{
    for (size_t i = 0; i + 4 <= len; i++)
    {
        if (mp3_frame_size(data + i, frame_size))
        {
            return (int)i;
        }
    }
    return -1;
}

static audio_format_t audio_detect_format(FILE *fp, const char *path)
{
    unsigned char header[12] = { 0 };
    size_t count = fread(header, 1, sizeof(header), fp);
    size_t frame_size;
    const char *extension = strrchr(path, '.');

    (void)fseek(fp, 0, SEEK_SET);
    if (count >= 8 && memcmp(header + 4, "ftyp", 4) == 0)
    {
        return AUDIO_FORMAT_M4A;
    }
    if (count >= 2 && header[0] == 0xFF && (header[1] & 0xF6) == 0xF0)
    {
        return AUDIO_FORMAT_AAC_ADTS;
    }
    if ((count >= 3 && memcmp(header, "ID3", 3) == 0)
        || mp3_find_frame(header, count, &frame_size) >= 0)
    {
        return AUDIO_FORMAT_MP3;
    }
    if (extension != NULL && strcasecmp(extension, ".m4a") == 0)
    {
        return AUDIO_FORMAT_M4A;
    }
    if (extension != NULL && strcasecmp(extension, ".aac") == 0)
    {
        return AUDIO_FORMAT_AAC_ADTS;
    }
    return AUDIO_FORMAT_UNKNOWN;
}

static esp_err_t audio_decode_mp3(FILE *fp, void *decoder,
                                  unsigned char *buffer, int16_t *pcm,
                                  size_t pcm_capacity)
{
    const unsigned char *input = buffer;
    size_t left = 0;
    audio_resampler_t resampler = { 0 };
    bool decoded = false;
    bool eof = false;

    while (!s_stop_req)
    {
        size_t frame_size;
        int sync;
        esp_audio_dec_in_raw_t raw = { 0 };
        esp_audio_dec_out_frame_t frame = { 0 };
        esp_audio_dec_info_t info = { 0 };
        esp_audio_err_t decode_result;

        if (!eof && left < AUDIO_INPUT_BUF_SIZE)
        {
            eof = audio_fill(fp, buffer, &input, &left) != 0;
        }
        if (left < 4)
        {
            break;
        }
        sync = mp3_find_frame(input, left, &frame_size);
        if (sync < 0)
        {
            if (eof)
            {
                break;
            }
            if (left > 3)
            {
                input += left - 3;
                left = 3;
            }
            continue;
        }
        input += sync;
        left -= (size_t)sync;
        if (left < frame_size)
        {
            if (eof || audio_fill(fp, buffer, &input, &left) != 0
                || left < frame_size)
            {
                break;
            }
        }

        raw.buffer = (uint8_t *)input;
        raw.len = frame_size;
        frame.buffer = (uint8_t *)pcm;
        frame.len = pcm_capacity * sizeof(int16_t);
        decode_result = esp_mp3_dec_decode(decoder, &raw, &frame, &info);
        input += frame_size;
        left -= frame_size;
        if (decode_result != ESP_AUDIO_ERR_OK)
        {
            ESP_LOGW(TAG, "MP3 frame decode failed: %d", (int)decode_result);
            continue;
        }
        if (!audio_write_esp_frame(&resampler, pcm, pcm_capacity,
                                   &frame, &info))
        {
            return ESP_ERR_INVALID_STATE;
        }
        decoded = true;
    }
    return s_stop_req ? ESP_ERR_INVALID_STATE
         : decoded ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

/**
 * Decode task body: waits for a PLAY notification, then streams the pending
 * file until it ends or a STOP request arrives.
 */
static void audio_decode_task(void *arg)
{
    (void)arg;

    int16_t pcm[AUDIO_PCM_BUF_SAMPLES];
    FILE *fp = NULL;


    for (;;)
    {
        uint32_t notify = 0;

        xTaskNotifyWait(0, UINT32_MAX, &notify, portMAX_DELAY);
        if ((notify & AUDIO_NOTIFY_PLAY) == 0)
        {
            continue;
        }

        fp = fopen(s_path, "rb");
        if (fp == NULL)
        {
            ESP_LOGE(TAG, "fopen(\"%s\") failed", s_path);
            s_playing = false;
            continue;
        }

        {
            audio_format_t format = audio_detect_format(fp, s_path);
            const char *format_name;
            esp_err_t decode_err;
            void *mp3_decoder = NULL;

            if (format == AUDIO_FORMAT_UNKNOWN)
            {
                ESP_LOGE(TAG, "unsupported audio format: %s", s_path);
                fclose(fp);
                fp = NULL;
                s_playing = false;
                continue;
            }
            if (format == AUDIO_FORMAT_MP3)
            {
                if (esp_mp3_dec_open(NULL, 0, &mp3_decoder)
                    != ESP_AUDIO_ERR_OK)
                {
                    ESP_LOGE(TAG, "Espressif MP3 decoder open failed");
                    fclose(fp);
                    fp = NULL;
                    s_playing = false;
                    continue;
                }
                ESP_LOGI(TAG, "MP3 decoder opened");
            }
            if (format == AUDIO_FORMAT_MP3
                && audio_skip_id3v2(fp) != ESP_OK)
            {
                ESP_LOGE(TAG, "failed to skip ID3v2 tag");
                (void)esp_mp3_dec_close(mp3_decoder);
                fclose(fp);
                fp = NULL;
                s_playing = false;
                continue;
            }

            format_name = format == AUDIO_FORMAT_M4A ? "M4A/AAC-LC"
                        : format == AUDIO_FORMAT_AAC_ADTS ? "AAC/ADTS"
                        : "MP3";
            audio_update_headphone_route(true);
            codec_es8156_unmute();
            s_playing = true;
            ESP_LOGI(TAG, "playing \"%s\" (%s)", s_path, format_name);

            if (format == AUDIO_FORMAT_M4A)
            {
                decode_err = audio_decode_m4a_aac(
                    fp, s_path, pcm, AUDIO_PCM_BUF_SAMPLES);
            }
            else if (format == AUDIO_FORMAT_AAC_ADTS)
            {
                decode_err = audio_decode_adts_aac(
                    fp, s_encoded_input, pcm, AUDIO_PCM_BUF_SAMPLES);
            }
            else
            {
                decode_err = audio_decode_mp3(
                    fp, mp3_decoder, s_encoded_input, pcm,
                    AUDIO_PCM_BUF_SAMPLES);
            }
            if (decode_err != ESP_OK && decode_err != ESP_ERR_INVALID_STATE)
            {
                ESP_LOGE(TAG, "%s playback failed: %s", format_name,
                         esp_err_to_name(decode_err));
            }
            if (mp3_decoder != NULL)
            {
                (void)esp_mp3_dec_close(mp3_decoder);
            }
        }

        codec_es8156_mute(true);
        (void)audio_i2s_clear();
        fclose(fp);
        fp = NULL;
        s_paused = false;
        s_stop_req = false;
        ESP_LOGI(TAG, "stopped \"%s\"", s_path);
        s_playing = false;
    }
}

esp_err_t audio_init(void)
{
    esp_err_t err;

    if (s_tx_chan != NULL)
    {
        ESP_LOGW(TAG, "already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Factory order: reset/identify the AW while BCLK/LRCLK are stopped.
     * SmartPA cold start is deferred until the I2S channel is active. */
    err = aw88194_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "AW88194 identification failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = codec_es8156_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "codec_es8156_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = audio_i2s_init();
    if (err != ESP_OK)
    {
        return err;
    }

    err = aw88194_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "AW88194 SmartPA start failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(audio_decode_task, "audio_dec", AUDIO_DECODE_STACK_BYTES,
                    NULL, AUDIO_DECODE_PRIORITY, &s_decode_task) != pdPASS)
    {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "audio ready (speaker + headphone, APLL 44100 Hz, 16-bit mono-left, I2S %d)",
             I2S_PORT);
    return ESP_OK;
}

esp_err_t audio_play(const char *path)
{
    FILE *probe;

    if (path == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_chan == NULL)
    {
        ESP_LOGE(TAG, "audio not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Pre-open to validate the path before touching the decode task. */
    probe = fopen(path, "rb");
    if (probe == NULL)
    {
        ESP_LOGE(TAG, "fopen(\"%s\") failed", path);
        return ESP_ERR_NOT_FOUND;
    }
    fclose(probe);

    /* A previous stream still running? Stop it (waits for the task to idle). */
    if (s_playing)
    {
        audio_stop();
    }

    strncpy(s_path, path, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = '\0';
    s_stop_req = false;
    s_paused = false;

    xTaskNotify(s_decode_task, AUDIO_NOTIFY_PLAY, eSetBits);
    return ESP_OK;
}

esp_err_t audio_stop(void)
{
    if (!s_playing)
    {
        s_stop_req = false;
        s_paused = false;
        return ESP_OK;
    }

    s_stop_req = true;
    xTaskNotify(s_decode_task, AUDIO_NOTIFY_STOP, eSetBits);

    /* Wait for the decode task to close the file and clear s_playing. */
    while (s_playing)
    {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

esp_err_t audio_pause(void)
{
    if (!s_playing)
    {
        return ESP_OK;
    }
    s_paused = true;
    return ESP_OK;
}

esp_err_t audio_resume(void)
{
    if (!s_playing)
    {
        return ESP_OK;
    }
    s_paused = false;
    return ESP_OK;
}

esp_err_t audio_set_volume(int vol)
{
    if (vol < 0)
    {
        vol = 0;
    }
    else if (vol > 100)
    {
        vol = 100;
    }

    s_volume = vol;
    return ESP_OK;
}

bool audio_is_playing(void)
{
    return s_playing;
}

bool audio_is_paused(void)
{
    return s_paused;
}

esp_err_t audio_play_blip(int freq_hz, int duration_ms)
{
    const int sample_rate = 44100;
    int frames;
    int fade;
    float step;
    int16_t *buf;

    if (s_tx_chan == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (freq_hz < 50 || freq_hz > 8000
        || duration_ms <= 0 || duration_ms > AUDIO_BLIP_MAX_MS)
    {
        return ESP_ERR_INVALID_ARG;
    }
    /* Content always wins: this path owns the DMA ring and clears it on the way
     * out, so it must never run against a live or paused stream. */
    if (s_playing)
    {
        return ESP_ERR_INVALID_STATE;
    }

    frames = sample_rate * duration_ms / 1000;
    fade = sample_rate * AUDIO_BLIP_FADE_MS / 1000;
    if (fade > frames / 2)
    {
        fade = frames / 2;
    }

    buf = calloc((size_t)frames, sizeof(int16_t));
    if (buf == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    /* Sine, matching the stock firmware's tone generator (a theta-accumulating
     * sine at 44.1 kHz, recovered at 0x401d8a28) rather than a square wave: a
     * square blip repeated per detent buzzes. */
    step = 2.0f * (float)M_PI * (float)freq_hz / (float)sample_rate;
    for (int i = 0; i < frames; i++)
    {
        float sample = sinf(step * (float)i) * (float)AUDIO_BLIP_AMPLITUDE;

        if (fade > 0 && i < fade)
        {
            sample = sample * (float)i / (float)fade;
        }
        else if (fade > 0 && i >= frames - fade)
        {
            sample = sample * (float)(frames - 1 - i) / (float)fade;
        }
        buf[i] = (int16_t)sample;
    }

    /* audio_write_pcm() applies the current volume as PCM gain, which is the
     * whole point: the blip is exactly as loud as content would be. */
    s_playing = true;
    s_stop_req = false;
    s_paused = false;
    (void)audio_write_pcm(buf, (size_t)frames);
    free(buf);
    (void)audio_i2s_clear();
    s_playing = false;
    return ESP_OK;
}

esp_err_t audio_play_tone(int freq_hz)
{
    const int sample_rate = 44100;
    const int on_ms = 400;
    const int off_ms = 300;
    const int on_frames = sample_rate * on_ms / 1000;
    const int off_frames = sample_rate * off_ms / 1000;
    const int period = sample_rate / freq_hz;
    const int half = period / 2;
    int16_t *buf;
    int total;
    int i;
    bool wrote_cycle = false;

    if (s_tx_chan == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (freq_hz < 50 || freq_hz > 8000)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_playing)
    {
        audio_stop();
    }

    total = on_frames + off_frames;
    buf = calloc((size_t)total, sizeof(int16_t));
    if (buf == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < on_frames; i++)
    {
        int16_t v = ((i % period) < half) ? AUDIO_TONE_AMPLITUDE
                                            : -AUDIO_TONE_AMPLITUDE;

        buf[i] = v;
    }

    s_playing = true;
    s_stop_req = false;
    s_paused = false;

    while (!s_stop_req)
    {
        if (!audio_write_pcm(buf, (size_t)total))
        {
            break;
        }
        if (!wrote_cycle)
        {
            ESP_LOGI(TAG, "tone DMA active: %d mono samples per cycle", total);
            wrote_cycle = true;
        }
    }

    free(buf);
    (void)audio_i2s_clear();
    s_playing = false;
    return ESP_OK;
}

static void audio_tone_task(void *arg)
{
    int freq_hz = (int)(intptr_t)arg;

    (void)audio_play_tone(freq_hz);
    s_tone_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_start_tone(int freq_hz)
{
    if (s_tx_chan == NULL || freq_hz < 50 || freq_hz > 8000)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tone_task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(audio_tone_task, "audio_tone", AUDIO_TONE_STACK_BYTES,
                    (void *)(intptr_t)freq_hz, AUDIO_TONE_PRIORITY,
                    &s_tone_task) != pdPASS)
    {
        s_tone_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "tone started: %d Hz, PCM amplitude %d",
             freq_hz, AUDIO_TONE_AMPLITUDE);
    return ESP_OK;
}
