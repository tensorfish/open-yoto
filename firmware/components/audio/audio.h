/*
 * audio.h — Yoto Player compressed-audio playback over I2S.
 *
 * Brings up the stock-matched APLL 44.1 kHz, 16-bit mono-left sink and both
 * audio outputs. A dedicated task streams MP3, standalone ADTS AAC, and
 * M4A/AAC-LC through Espressif decoders, source-metadata-driven downmix and
 * resampling, then bounded-volume I2S DMA writes.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * Initialize the audio path: reset/identify the speaker amp while clocks are
 * stopped, initialize the ES8156 DAC, install the stock APLL/mono-left I2S TX
 * channel, then load/start the AW88194A SmartPA. Must be called after
 * iox_init() so the I2C bus is available.
 *
 * @return ESP_OK only when the speaker, codec, I2S channel, and decode task
 *         are ready; otherwise the corresponding initialization error.
 */
esp_err_t audio_init(void);

/**
 * Begin playing a supported compressed-audio file from the VFS. Content
 * signatures select MP3, ADTS AAC, or M4A/AAC-LC; the extension is only a
 * fallback. Decoding runs in a dedicated FreeRTOS task, so this call returns
 * after scheduling playback.
 *
 * If another file is already playing it is stopped first.
 *
 * path NUL-terminated VFS path to the audio file.
 *
 * return ESP_OK on success, ESP_ERR_INVALID_ARG on a NULL path,
 *        ESP_ERR_NOT_FOUND if the file cannot be opened, or the underlying
 *        I2S error.
 */
esp_err_t audio_play(const char *path);

/**
 * Stop playback. Signals the decode task to stop and blocks until it has
 * closed the file and returned to its idle wait. Idempotent.
 *
 * return ESP_OK when playback has stopped.
 */
esp_err_t audio_stop(void);

/**
 * Pause playback. The decode task keeps its file position but stops pushing
 * PCM to the I2S DMA until audio_resume() is called. No-op if not playing.
 *
 * return ESP_OK on success.
 */
esp_err_t audio_pause(void);

/**
 * Resume a paused stream. No-op if not paused.
 *
 * return ESP_OK on success.
 */
esp_err_t audio_resume(void);

/**
 * Set the playback volume.
 *
 * @param[in] vol 0..100 percent; clamped and applied as bounded PCM gain
 *                 before I2S transmission, affecting speaker and headphone.
 * @return ESP_OK.
 */
esp_err_t audio_set_volume(int vol);

/**
 * Return true while a file is being played (including when paused).
 */
bool audio_is_playing(void);

/**
 * Return true while playback is paused (audio_is_playing() is also true).
 */
bool audio_is_paused(void);


/**
 * Generate a continuous square-wave beep (test aid): 400 ms on at freq_hz,
 * 300 ms off, repeating until audio_stop() is called. Blocks the calling
 * task for the duration of the beeping.
 *
 * @param freq_hz  beep frequency in Hz (50..8000).
 * @return ESP_OK once stopped, or an esp_err_t on invalid state/argument.
 */
esp_err_t audio_play_tone(int freq_hz);

/**
 * Start the repeating square-wave tone in a dedicated task. Unlike
 * audio_play_tone(), this returns immediately so encoder volume events remain
 * responsive. Stop it with audio_stop().
 *
 * @param[in] freq_hz Tone frequency (50..8000).
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, or ESP_ERR_NO_MEM.
 */
esp_err_t audio_start_tone(int freq_hz);

/**
 * Play one short square-wave blip at the current volume. This is the audible
 * half of volume feedback: with nothing playing, a volume change is otherwise
 * silent. Blocks the calling task for roughly duration_ms, and is refused with
 * ESP_ERR_INVALID_STATE while a stream is loaded (playing or paused) because it
 * owns and then clears the I2S DMA ring.
 *
 * @param[in] freq_hz     Blip frequency (50..8000).
 * @param[in] duration_ms Blip length in ms (1..250).
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, or ESP_ERR_NO_MEM.
 */
esp_err_t audio_play_blip(int freq_hz, int duration_ms);