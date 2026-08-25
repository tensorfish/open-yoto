/*
 * Host-test stub for audio.h. Mirrors the prototypes app_main.c uses; the host
 * test provides no-op definitions and a test-settable audio_is_playing().
 */
#ifndef HOST_STUB_AUDIO_H
#define HOST_STUB_AUDIO_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t audio_init(void);
esp_err_t audio_play(const char *path);
esp_err_t audio_stop(void);
esp_err_t audio_pause(void);
esp_err_t audio_resume(void);
esp_err_t audio_set_volume(int vol);
bool audio_is_playing(void);
bool audio_is_paused(void);
esp_err_t audio_play_blip(int freq_hz, int duration_ms);

#endif /* HOST_STUB_AUDIO_H */
