/*
 * audio.c — Yoto Player MP3 player (I2S std TX -> ES8156 headphone DAC).
 *
 * audio_init() installs the I2S standard-mode master TX channel at 44100 Hz,
 * 16-bit, stereo on I2S_NUM_0 and brings up the ES8156 headphone DAC. A
 * persistent FreeRTOS task then decodes MP3 files (Helix decoder) into
 * interleaved int16 PCM and writes it to the I2S TX DMA.
 *
 * Playback control (play/stop/pause/resume) is shared state checked by the
 * decode task. A task notification carries the stop/play requests so the task
 * can block on its idle wait and still be woken from a paused sleep.
 */
#include "audio.h"

#include "board_pins.h"
#include "codec_es8156.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mp3dec.h"

#include <string.h>

static const char *TAG = "audio";

/* Task notification bits (a bitmask in the task's notification value). */
#define AUDIO_NOTIFY_STOP   (1u << 0)
#define AUDIO_NOTIFY_PLAY   (1u << 1)

/* Decoder input work buffer. Sized for a full frame plus the Helix main-data
 * bit reservoir, so a frame never straddles the read window under normal
 * bitrates. */
#define AUDIO_INPUT_BUF_SIZE    2048

/* Maximum PCM samples per decoded frame: MPEG1 layer 3 stereo = 1152 samples
 * per channel = 2304 interleaved samples. */
#define AUDIO_PCM_BUF_SAMPLES   2304

/* I2S DMA write timeout. Kept short so the decode task stays responsive to
 * stop/pause while still providing natural backpressure on a full DMA. */
#define AUDIO_I2S_WRITE_TICK_MS 20

/* Decode task stack size in bytes (ESP-IDF FreeRTOS convention). */
#define AUDIO_DECODE_STACK_BYTES 10240

/* Decode task priority. Below the encoder (5) but above idle (0). */
#define AUDIO_DECODE_PRIORITY   4

/* Path handed from audio_play() to the decode task. */
#define AUDIO_PATH_MAX          128

/* TX channel handle for the i2s_std driver. */
static i2s_chan_handle_t s_tx_chan = NULL;

/* Decode task handle, created in audio_init(). */
static TaskHandle_t s_decode_task = NULL;

/* Shared playback state. s_stop_req and s_paused are written from the calling
 * thread and read (polled) by the decode task; s_playing is written by the
 * decode task and read by audio_is_playing(). Volatile makes those cross-task
 * accesses well-defined. */
static volatile bool s_playing = false;
static volatile bool s_stop_req = false;
static volatile bool s_paused = false;

/* File path for the current playback request, installed before the PLAY
 * notification is issued to the decode task. */
static char s_path[AUDIO_PATH_MAX];

/**
 * Configure the I2S std TX channel: master, 44100 Hz, 16-bit, Philips/I2S,
 * stereo, on the board I2S pins.
 *
 * return ESP_OK on success, otherwise the first I2S driver error.
 */
static esp_err_t audio_i2s_init(void)
{
    esp_err_t err;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT,
                                                            I2S_ROLE_MASTER);
    err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    /* MCLK = 256 * fs (the ES8156 expects 256 fs), 16-bit stereo Philips. */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
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

    ESP_LOGI(TAG, "I2S TX enabled (mclk=%d bclk=%d lrclk=%d dout=%d, %d Hz)",
             PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT,
             I2S_SAMPLE_RATE_HZ);
    return ESP_OK;
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
 * Write a block of interleaved 16-bit PCM to the I2S TX DMA, retrying while
 * the DMA is full and honoring pause/stop in between attempts.
 *
 * pcm    PCM samples (interleaved stereo).
 * frames number of 16-bit samples in pcm.
 *
 * return true if the block was fully written, false if playback stopped while
 *        waiting.
 */
static bool audio_write_pcm(const int16_t *pcm, size_t frames)
{
    size_t offset = 0;

    while (offset < frames)
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

        err = i2s_channel_write(s_tx_chan, pcm + offset,
                                (frames - offset) * sizeof(int16_t),
                                &written,
                                pdMS_TO_TICKS(AUDIO_I2S_WRITE_TICK_MS));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
        {
            ESP_LOGE(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err));
            return false;
        }
        offset += written / sizeof(int16_t);
    }
    return true;
}

/**
 * Decode task body: waits for a PLAY notification, then streams the pending
 * file until it ends or a STOP request arrives.
 */
static void audio_decode_task(void *arg)
{
    (void)arg;

    HMP3Decoder decoder = NULL;
    unsigned char inbuf[AUDIO_INPUT_BUF_SIZE];
    int16_t pcm[AUDIO_PCM_BUF_SAMPLES];
    FILE *fp = NULL;

    decoder = MP3InitDecoder();
    if (decoder == NULL)
    {
        ESP_LOGE(TAG, "MP3InitDecoder failed");
    }

    for (;;)
    {
        /* Block until a play/stop request; the notification value is only a
         * wake-up, the state flags (s_stop_req, s_paused) drive behavior. */
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (decoder == NULL)
        {
            ESP_LOGE(TAG, "decoder unavailable; ignoring play request");
            s_playing = false;
            continue;
        }

        fp = fopen(s_path, "rb");
        if (fp == NULL)
        {
            ESP_LOGE(TAG, "fopen(\"%s\") failed", s_path);
            s_playing = false;
            continue;
        }

        if (audio_skip_id3v2(fp) != ESP_OK)
        {
            ESP_LOGE(TAG, "failed to skip ID3v2 tag");
            fclose(fp);
            fp = NULL;
            s_playing = false;
            continue;
        }

        codec_es8156_unmute();
        s_playing = true;
        ESP_LOGI(TAG, "playing \"%s\"", s_path);

        {
            const unsigned char *in_ptr = inbuf;
            size_t left = 0;

            while (!s_stop_req)
            {
                int sync;
                int rc;
                MP3FrameInfo info;
                const unsigned char *before;
                size_t consumed;
                size_t out_samples;

                if (left < 4 && audio_fill(fp, inbuf, &in_ptr, &left) != 0)
                {
                    break;   /* end of file */
                }

                sync = MP3FindSyncWord(in_ptr, (int)left);
                if (sync < 0)
                {
                    if (audio_fill(fp, inbuf, &in_ptr, &left) != 0)
                    {
                        break;   /* end of file with no complete frame */
                    }
                    continue;
                }
                in_ptr += sync;
                left -= (size_t)sync;

                before = in_ptr;
                rc = MP3Decode(decoder, &in_ptr, &left, pcm, 0);
                consumed = (size_t)(in_ptr - before);

                if (rc == ERR_MP3_NONE)
                {
                    MP3GetLastFrameInfo(decoder, &info);
                    out_samples = (size_t)info.outputSamps;
                    if (out_samples > AUDIO_PCM_BUF_SAMPLES)
                    {
                        out_samples = AUDIO_PCM_BUF_SAMPLES;
                    }
                    if (!audio_write_pcm(pcm, out_samples))
                    {
                        break;
                    }
                }
                else
                {
                    /* MP3Decode already advanced in_ptr (and decremented left)
                     * by the bytes it consumed before reporting the error. On a
                     * bad frame header it consumes nothing, so force a step
                     * past this sync word to guarantee forward progress, then
                     * rescan for the next frame. */
                    if (consumed < 2)
                    {
                        size_t step = 2 - consumed;
                        in_ptr += step;
                        left -= step;
                    }
                    if (left == 0)
                    {
                        break;
                    }
                    ESP_LOGD(TAG, "MP3Decode error %d, resyncing", rc);
                }
            }
        }

        codec_es8156_mute(true);
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

    err = audio_i2s_init();
    if (err != ESP_OK)
    {
        return err;
    }

    err = codec_es8156_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "codec_es8156_init failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(audio_decode_task, "audio_dec", AUDIO_DECODE_STACK_BYTES,
                    NULL, AUDIO_DECODE_PRIORITY, &s_decode_task) != pdPASS)
    {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "audio ready (44100 Hz, 16-bit stereo, I2S %d)", I2S_PORT);
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
    return codec_es8156_set_volume(vol);
}

bool audio_is_playing(void)
{
    return s_playing;
}
