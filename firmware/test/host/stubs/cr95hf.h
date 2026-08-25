/*
 * Host-test stub for cr95hf.h. Mirrors the types and prototypes app_main.c
 * references; the host test provides no-op/false definitions.
 */
#ifndef HOST_STUB_CR95HF_H
#define HOST_STUB_CR95HF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define CR95HF_UID_MAX 10
#define CR95HF_URL_MAX 200

typedef enum {
    CR95HF_CARD_BLANK,
    CR95HF_CARD_URI,
    CR95HF_CARD_NON_URI,
    CR95HF_CARD_LOCKED,
    CR95HF_CARD_UNREADABLE,
} cr95hf_card_state_t;

typedef struct {
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

esp_err_t cr95hf_init(void);
bool cr95hf_poll_card(uint8_t *uid, uint8_t *uid_len, char *url,
                      size_t url_cap, cr95hf_card_info_t *info);
esp_err_t cr95hf_write_url(const char *url, const uint8_t *expected_uid,
                           uint8_t expected_uid_len);

#endif /* HOST_STUB_CR95HF_H */
