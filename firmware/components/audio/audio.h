/*
 * audio.h — Yoto Player MP3 player (I2S std TX -> ES8156 headphone DAC).
 *
 * Brings up the I2S master TX channel (44100 Hz, 16-bit, stereo) on I2S_NUM_0
 * and the ES8156 headphone DAC, then streams MP3 files from the FatFS VFS.
 * Playback is decoded with the vendored Helix MP3 decoder (libhelix-mp3) in a
 * dedicated FreeRTOS task and written to the I2S DMA as interleaved int16 PCM.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * Initialize the audio path: install the I2S std TX channel (44100 Hz, 16-bit
 * stereo, master) on I2S_NUM_0 and bring up the ES8156 headphone DAC. Must be
 * called after iox_init() so the I2C bus is available.
 *
 * return ESP_OK on success, otherwise the first I2S or codec error.
 */
esp_err_t audio_init(void);

/**
 * Begin playing an MP3 file. The file is opened with fopen()/fread() on the
 * VFS (path may be absolute, e.g. "/sdcard/media/a.mp3", or relative to the
 * current working directory), the ID3v2 tag is skipped, and frames are
 * decoded (MP3FindSyncWord/MP3Decode) into interleaved 16-bit PCM pushed to
 * the I2S TX DMA. Decoding runs in a dedicated FreeRTOS task, so this call
 * returns once playback has been started.
 *
 * If another file is already playing it is stopped first.
 *
 * path NUL-terminated path to the MP3 file.
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
 * vol 0..100 percent; clamped to that range and applied to the ES8156 DAC.
 *
 * return ESP_OK on success, otherwise the codec I2C error.
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