/*
 * board_pins.h — Yoto Player (ESP32) pin map.
 *
 * Authoritative pin assignments recovered from the stock firmware's embedded
 * hardware config (hwconfig_05, the latest 2-IO-expander revision). These are
 * the values the factory firmware itself consumes at boot.
 *
 * NOTE: the Yoto shipped several hardware revisions with DIFFERENT pins. This
 * header targets the latest revision (2x PI4IOE5V6416 IO expanders, HT16D35x
 * LED matrix, UART NFC, SDMMC 1-bit, CW2215B gauge, aw881xx + ES8156 audio).
 * See docs/hardware.md for the full variant table if your unit differs.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "driver/sdmmc_host.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"

/* ------------------------------------------------------------------ I2C -- */
#define PIN_I2C_SDA                 GPIO_NUM_21
#define PIN_I2C_SCL                 GPIO_NUM_25
#define I2C_MASTER_FREQ_HZ          100000
#define I2C_PORT                    I2C_NUM_0

/* I2C device addresses (7-bit) */
#define I2C_ADDR_IOX_0              0x20   /* PI4IOE5V6416 #0 (buttons, charger, codec ctrl) */
#define I2C_ADDR_IOX_1              0x21   /* PI4IOE5V6416 #1 (display CS, power ctrl)      */
#define I2C_ADDR_FUEL_GAUGE         0x64   /* CW2215B battery fuel gauge                    */
#define I2C_ADDR_CHARGER            0x6B   /* SGM41513 charger                              */
#define I2C_ADDR_HP_DAC             0x09   /* ES8156 headphone DAC                          */
#define I2C_ADDR_SPKR_AMP_L         0x34   /* aw881xx speaker amp (left)                    */
#define I2C_ADDR_SPKR_AMP_R         0x37   /* aw881xx speaker amp (right)                   */
#define I2C_ADDR_RTC                0x51   /* IT8563 RTC                                    */

/* ------------------------------------------------------------------ SPI -- */
/* Display bus (HT16D35x LED matrix): shared mosi/miso/sclk, CS via IO expander. */
#define PIN_SPI_MOSI                GPIO_NUM_22
#define PIN_SPI_MISO                GPIO_NUM_26
#define PIN_SPI_SCLK                GPIO_NUM_23
#define SPI_HOST                    SPI2_HOST

/* ----------------------------------------------------------------- SDMMC -- */
/* 1-bit SD card (hwconfig_05 "sd1"). 4-bit variant adds d1=4, d2=12, d3=13.   */
#define PIN_SD_CLK                  GPIO_NUM_14
#define PIN_SD_CMD                  GPIO_NUM_15
#define PIN_SD_D0                   GPIO_NUM_2
#define SD_SLOT_CONFIG              SDMMC_SLOT_CONFIG_DEFAULT()

/* ------------------------------------------------------------------ NFC -- */
/* CR95HF over UART (hwconfig_05 "nfc": type=uart).                            */
#define PIN_NFC_RX                  GPIO_NUM_32
#define PIN_NFC_TX                  GPIO_NUM_33
#define NFC_UART_PORT               UART_NUM_2
#define NFC_UART_BAUD               57600

/* ------------------------------------------------------------------ I2S -- */
#define PIN_I2S_MCLK                GPIO_NUM_0
#define PIN_I2S_BCLK                GPIO_NUM_5
#define PIN_I2S_LRCLK               GPIO_NUM_18
#define PIN_I2S_DOUT                GPIO_NUM_19
#define I2S_PORT                    I2S_NUM_0
#define I2S_SAMPLE_RATE_HZ          44100

/* ------------------------------------------------------------------ ADC -- */
/* Battery VBAT (ADC1 CH3) + light sensor (ADC1 CH0) + IR temp (ADC1 CH7).     */
#define PIN_ADC_BAT_VBAT            GPIO_NUM_39   /* ADC1 channel 3 */
#define PIN_ADC_LIGHT               GPIO_NUM_36   /* ADC1 channel 0 */
#define PIN_ADC_IR_TEMP             GPIO_NUM_35   /* ADC1 channel 7 */
#define ADC_CH_BAT                  ADC_CHANNEL_3
#define ADC_CH_LIGHT                ADC_CHANNEL_0
#define ADC_CH_IR_TEMP              ADC_CHANNEL_7

/* -------------------------------------------------------------- buttons --- */
/* Rotary encoders (quadrature on ESP32 GPIO) + push buttons (via IO expander) */
#define PIN_ENC0_A                  GPIO_NUM_26   /* shared with SPI MISO */
#define PIN_ENC0_B                  GPIO_NUM_13
#define PIN_ENC1_A                  GPIO_NUM_27
#define PIN_ENC1_B                  GPIO_NUM_4

/* ------------------------------------------------------- IO expander ---- */
/*
 * IO-expander pin encoding. The stock firmware names pins "IOX.P.N" where P is
 * the global port 0..3 and N the bit 0..7. Two PI4IOE5V6416 (16-bit each) are
 * mapped as:  P 0..1 -> expander 0 (ports 0,1),  P 2..3 -> expander 1 (ports 0,1).
 *
 * Encoded byte: (expander << 6) | (port_within_exp << 4) | bit.
 */
#define IOX_PIN(exp, port, bit)     (((exp) << 6) | ((port) << 4) | (bit))
#define IOX_EXP(x)                  (((x) >> 6) & 0x03)
#define IOX_PORT(x)                 (((x) >> 4) & 0x03)
#define IOX_BIT(x)                  ((x) & 0x0F)

#define PIN_IOX_INT                 GPIO_NUM_34

/* expander 0 = ports 0..1 */
#define IOX_BTN_ENC0_PUSH           IOX_PIN(0, 0, 5)   /* IOX.0.5 */
#define IOX_BTN_ENC1_PUSH           IOX_PIN(0, 0, 4)   /* IOX.0.4 */
#define IOX_BAT_ALERT               IOX_PIN(0, 0, 6)   /* IOX.0.6 */
#define IOX_BTN_POWER               IOX_PIN(0, 1, 3)   /* IOX.1.3 */
#define IOX_CHG_STAT                IOX_PIN(0, 1, 4)   /* IOX.1.4 */
#define IOX_AUDIO_HPDETECT          IOX_PIN(0, 1, 1)   /* IOX.1.1 */

/* expander 1 = ports 2..3 */
#define IOX_DISP_CSN0               IOX_PIN(1, 0, 0)   /* IOX.2.0 */
#define IOX_DISP_CSN1               IOX_PIN(1, 0, 1)   /* IOX.2.1 */
#define IOX_DISP_CSN2               IOX_PIN(1, 0, 2)   /* IOX.2.2 */
#define IOX_DISP_CSN3               IOX_PIN(1, 0, 3)   /* IOX.2.3 */
#define IOX_AUDIO_PACTRL            IOX_PIN(1, 0, 4)   /* IOX.2.4 */
#define IOX_POWER_PWREN             IOX_PIN(1, 0, 5)   /* IOX.2.5 */
#define IOX_POWER_VINHOLD           IOX_PIN(1, 1, 1)   /* IOX.3.1 */
#define IOX_POWER_VOUTEN            IOX_PIN(1, 1, 3)   /* IOX.3.3 */
