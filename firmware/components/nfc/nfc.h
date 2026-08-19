/*
 * nfc.h — ST CR95HF NFC tag reader (over UART).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * Initialize the CR95HF UART link and bring the transceiver to a known state.
 * Returns ESP_OK on success (bus is up and usable), or the underlying UART
 * driver error.
 */
esp_err_t nfc_init(void);

/**
 * Poll for a tag and copy its UID.
 *
 * @param uid      output buffer for the UID bytes
 * @param uid_len  in: capacity of @p uid; out: number of UID bytes written
 * @return         true if a UID was read, false otherwise
 */
bool nfc_poll(uint8_t *uid, uint8_t *uid_len);
