/*
 * aw88194.h — AW88194A smart speaker amplifier.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * Apply the factory LOW/HIGH hardware reset and identify the rev #04 mono
 * AW88194A before I2S clocks are enabled.
 */
esp_err_t aw88194_init(void);

/**
 * With I2S clocks active, run the recovered factory SmartPA cold start:
 * register table, DSP firmware/config, VCALB, status/PLL checks, and unmute.
 */
esp_err_t aw88194_start(void);

/**
 * Power down or resume the SmartPA output while preserving its programmed DSP
 * state. Used to keep the speaker silent while headphones are connected.
 */
esp_err_t aw88194_set_muted(bool muted);
