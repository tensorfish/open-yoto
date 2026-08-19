/*
 * audio.h — Yoto Player audio output (I2S -> ES8156 headphone DAC, aw881xx amp).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Bring up the I2S (i2s_std) TX channel, enable the speaker amp rail via the
 * IO expander, and probe the ES8156 DAC + aw881xx speaker amps on the shared
 * I2C bus (installed by iox_init). Codec register init is still TODO.
 */
esp_err_t audio_init(void);

/**
 * Stream PCM samples (interleaved 16-bit stereo, 44100 Hz) out the I2S DMA.
 * TODO: implement the DMA write.
 */
esp_err_t audio_play(const int16_t *samples, size_t n);
