/*
 * cr95hf.h — ST CR95HF NFC transceiver driver (UART transport, ISO14443-3A).
 *
 * Owns the UART link to the CR95HF (57600 8-N-2) and implements Type 2 tag
 * (NTAG / MIFARE Ultralight) NDEF discovery: reads a tag UID and its URI-record
 * URL, and writes a URL back into the tag's NDEF area.
 *
 * Requires the board component for the NFC pin map (PIN_NFC_RX / PIN_NFC_TX).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * Bring up the CR95HF UART link (UART2, 57600 8-N-2, TX=GPIO33 RX=GPIO32),
 * install the UART driver, and select the ISO14443A RF protocol.
 *
 * @return ESP_OK on success, or the underlying UART driver / probe error.
 */
esp_err_t cr95hf_init(void);

/**
 * Poll for a Type A tag, then read its UID and (optionally) its NDEF URL.
 *
 * Performs a full ISO14443-3A activation (REQA -> anticollision -> select,
 * including cascade level 2 for 7-byte UIDs). The tag must still be in the
 * field when called; this function does not block waiting for one.
 *
 * @param uid      output buffer for the UID bytes (4 or 7 bytes)
 * @param uid_len  in: capacity of @p uid; out: number of UID bytes written
 * @param url      optional output buffer for the decoded URL; may be NULL
 * @param url_cap  capacity of @p url in bytes (including the NUL terminator);
 *                 ignored when @p url is NULL
 * @return         true if a tag was activated and its UID read, false otherwise
 */
bool cr95hf_poll(uint8_t *uid, uint8_t *uid_len, char *url, size_t url_cap);

/**
 * Write a URL as an NDEF URI record onto a tag already present in the field.
 *
 * The URL is split into a standard URI prefix code (http://, https://, ...)
 * and remainder to keep the record compact, wrapped in an NDEF-message TLV and
 * written as 4-byte pages starting at the user-data page.
 *
 * @param url  NUL-terminated URL to store (non-empty)
 * @return     ESP_OK on success; ESP_ERR_NOT_FOUND if no tag is in the field,
 *             ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE for a bad/oversized
 *             URL, or ESP_FAIL if the tag NAKs a page write.
 */
esp_err_t cr95hf_write_url(const char *url);
