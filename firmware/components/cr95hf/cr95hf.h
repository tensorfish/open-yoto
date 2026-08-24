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

#define CR95HF_UID_MAX 10
#define CR95HF_URL_MAX 200

typedef enum
{
    CR95HF_CARD_BLANK,
    CR95HF_CARD_URI,
    CR95HF_CARD_NON_URI,
    CR95HF_CARD_LOCKED,
    CR95HF_CARD_UNREADABLE,
} cr95hf_card_state_t;

/*
 * Card data captured during one activation. raw_ndef contains the formatted
 * Type-2 user area when it could be read; no allocation is performed.
 */
typedef struct
{
    cr95hf_card_state_t state;
    uint8_t sak;
    uint8_t cc[4];
    uint8_t lock0;
    uint8_t lock1;
    size_t capacity;
    bool writable;
    size_t raw_ndef_len;
    uint8_t raw_ndef[256];
} cr95hf_card_info_t;

/**
 * Bring up the CR95HF UART link (UART1, 57600 8-N-2, ESP TX=GPIO32 and
 * ESP RX=GPIO33), synchronize it with Echo, and select ISO14443A.
 *
 * @return ESP_OK on success, or the underlying UART/CR95HF protocol error.
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
 * Poll a Type A tag and capture its Type-2/NDEF state for serial diagnostics.
 *
 * A card is blank only when its capability container is all zeroes or when
 * a valid, writable Type-2 NDEF area contains no NDEF records. Non-URI,
 * malformed, locked, and unreadable cards are never classified as blank.
 *
 * @param info optional diagnostics output; may be NULL
 * @return true if a tag was activated and its UID read, false otherwise
 */
bool cr95hf_poll_card(uint8_t *uid, uint8_t *uid_len, char *url,
                      size_t url_cap, cr95hf_card_info_t *info);

/**
 * Write a URL to the expected Type A tag currently present in the field.
 *
 * Activation, UID comparison, safe blank-MF0UL11 formatting, page writes, and
 * URL read-back verification are serialized under the UART mutex. This
 * prevents replacing the scanned tag with another tag between capture/write.
 *
 * @param url          NUL-terminated URL to store (non-empty)
 * @param expected_uid UID captured by the admin scan
 * @param expected_uid_len expected UID length
 * @return                 ESP_OK on verified success; ESP_ERR_NOT_FOUND when
 *                         no tag is present; ESP_ERR_INVALID_STATE when a
 *                         different tag is present; ESP_ERR_NOT_SUPPORTED for
 *                         an unknown blank tag; lock/access, size, transport,
 *                         NAK, or read-back errors otherwise.
 */
esp_err_t cr95hf_write_url(const char *url, const uint8_t *expected_uid,
                           uint8_t expected_uid_len);
