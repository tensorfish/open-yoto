/*
 * codec_es8156.h — ES8156 headphone DAC driver (I2C).
 *
 * The ES8156 is a stereo headphone DAC on the shared I2C bus. The bus itself
 * is installed by iox_init(); this driver only issues register writes over it
 * via i2c_master_write_to_device at I2C_ADDR_HP_DAC (0x09, the board's strap
 * vs. the chip's 0x08 default). It brings the part into software (I2C) mode,
 * configures the 16-bit I2S slave input, and exposes percentage volume plus
 * mute/unmute.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * Initialize the ES8156 headphone DAC: reset, power up, select the 16-bit
 * I2S (Philips) slave input, enable the DAC, and apply the default volume.
 * Must be called after iox_init() so the I2C bus is available.
 *
 * return ESP_OK on success, otherwise the error from the first failed I2C
 *        register write.
 */
esp_err_t codec_es8156_init(void);

/**
 * Set the headphone volume.
 *
 * vol 0..100 percent, mapped linearly onto the DAC volume register 0x14
 *      (0.5 dB steps). Values outside 0..100 are clamped.
 *
 * return ESP_OK on success, otherwise the I2C write error.
 */
esp_err_t codec_es8156_set_volume(int vol);

/**
 * Mute or unmute the headphone output (digital DAC mute + analog output mute).
 *
 * mute true mutes, false unmutes.
 *
 * return ESP_OK on success, otherwise the I2C write error.
 */
esp_err_t codec_es8156_mute(bool mute);

/**
 * Unmute the headphone output.
 *
 * return ESP_OK on success, otherwise the I2C write error.
 */
esp_err_t codec_es8156_unmute(void);
