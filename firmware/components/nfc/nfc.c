/*
 * nfc.c — ST CR95HF NFC tag reader driver (UART transport).
 *
 * The CR95HF is a 13.56 MHz NFC transceiver. This driver owns the UART link
 * (bus + peripheral setup) and the tag-polling entry point; the exact CR95HF
 * host-protocol framing and the ISO14443-3A activation handshake are marked
 * as TODO with datasheet references, since they cannot be verified against
 * this repo's sources.
 *
 * Transport: UART2, TX=GPIO33 RX=GPIO32, 57600 8-N-1 (board_pins.h).
 */
#include "nfc.h"
#include "board_pins.h"

#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "nfc";

/* ------------------------------------------------------------------ CR95HF
 * Host-protocol commands and frame layout. Reference: ST CR95HF datasheet
 * DS10311 and application note AN3954 ("CR95HF — how to control it"). These
 * opcodes are the minimum set needed for tag discovery:
 *
 *   0x01  Idn            — read device ID/ROM (identity probe)
 *   0x02  ProtocolSelect — select ISO14443A/B, ISO15693, ... RF protocol
 *   0x04  SendRecv       — exchange raw bytes with a tag (REQA/anticollision)
 *   0x07  Idle           — put the transceiver in the Idle state
 *
 * TODO(DS10311/AN3954): verify the exact on-wire frame layout and checksum
 * before exchanging frames. The framing is spec'd as:
 *
 *     SOF(0x00) | LEN | CMD | DATA | EOF(0x00)
 *
 * where LEN counts the CMD + DATA bytes. Confirm that LEN semantics and any
 * trailing CRC against the datasheet first.
 */
#define NFC_CMD_IDN              0x01
#define NFC_CMD_PROTOCOL_SELECT  0x02
#define NFC_CMD_SENDRECV         0x04
#define NFC_CMD_IDLE             0x07

#define NFC_SOF                  0x00   /* start of frame */
#define NFC_EOF                  0x00   /* end of frame */

/* ISO14443-3A tag activation bytes (sent inside SendRecv) */
#define NFC_REQA                 0x26   /* request type A */
#define NFC_ANTICOLL             0x93   /* anticollision level 1 */
#define NFC_SEL_CASCADE_L1       0x20   /* select cascade level 1 (anticollision) */
#define NFC_SEL_UID              0x70   /* select by UID */

#define NFC_UART_RX_BUF_SIZE     256
#define NFC_UID_MAX              10    /* 4/7/10-byte ISO14443-3A UIDs */

/* -------------------------------------------------------------------- init */

esp_err_t nfc_init(void)
{
    esp_err_t err;

    uart_config_t cfg = {
        .baud_rate  = NFC_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    err = uart_param_config(NFC_UART_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = uart_set_pin(NFC_UART_PORT, PIN_NFC_TX, PIN_NFC_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }

    /* RX ring buffer for polling reads; no event queue, no interrupt flags. */
    err = uart_driver_install(NFC_UART_PORT, NFC_UART_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    /* TODO(DS10311/AN3954): bring the CR95HF to a known state here — send
     * Idn (identity probe) then ProtocolSelect(ISO14443A) once the frame
     * layout above is verified against the datasheet. */

    ESP_LOGI(TAG, "CR95HF UART up (TX=%d RX=%d @ %d baud)",
             PIN_NFC_TX, PIN_NFC_RX, NFC_UART_BAUD);
    return ESP_OK;
}

/* -------------------------------------------------------------------- poll */

bool nfc_poll(uint8_t *uid, uint8_t *uid_len)
{
    if (uid == NULL || uid_len == NULL || *uid_len == 0) {
        return false;
    }

    /*
     * TODO(DS10311/AN3954): drive the full ISO14443-3A activation before
     * reading — REQA 0x26 -> anticollision 0x93 0x20 -> select 0x93 0x70 —
     * via ProtocolSelect(ISO14443A) + SendRecv, then extract the UID from the
     * response frame. Until that handshake is implemented, this stub drains
     * whatever length-prefixed UID the chip has already framed on the UART.
     */
    uint8_t len = 0;
    int got = uart_read_bytes(NFC_UART_PORT, &len, 1, pdMS_TO_TICKS(100));
    if (got != 1 || len == 0) {
        return false;
    }

    uint8_t cap = *uid_len;
    if (len > cap) {
        len = cap;                    /* caller buffer too small — truncate */
    }
    if (len > NFC_UID_MAX) {
        len = NFC_UID_MAX;
    }

    got = uart_read_bytes(NFC_UART_PORT, uid, len, pdMS_TO_TICKS(200));
    if (got != len) {
        return false;
    }

    ESP_LOGI(TAG, "tag UID (%d bytes):", len);
    for (int i = 0; i < len; i++) {
        ESP_LOGI(TAG, "  uid[%d] = 0x%02x", i, uid[i]);
    }

    *uid_len = len;
    return true;
}
