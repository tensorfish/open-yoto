/*
 * cr95hf.c — ST CR95HF NFC transceiver driver (UART transport).
 *
 * The cold-start Echo uses the recovered 0x00 prefix:
 *   host -> CR95HF Echo: 0x00 0x55
 * Other host commands use the documented [command][length][data...] framing.
 * CR95HF replies are [result code][length][data...].
 *
 * The final byte of every SendRecv payload is a transmit-flag byte: 0x07
 * emits a 7-bit short frame, 0x08 emits a whole-byte frame without CRC-A,
 * and 0x28 emits a whole-byte frame with CRC-A.
 */
#include "cr95hf.h"
#include "board_pins.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "cr95hf";

/* ---------------------------------------------------------------- CR95HF
 * Host-interface command codes and result codes (DS10311 / AN3954).
 */
#define CR95HF_CMD_IDN              0x01
#define CR95HF_CMD_PROTOCOL_SELECT  0x02
#define CR95HF_CMD_SENDRECV         0x04
#define CR95HF_CMD_WRREG            0x09
#define CR95HF_CMD_ECHO             0x55

#define CR95HF_CODE_SUCCESS         0x00   /* command executed successfully  */
#define CR95HF_CODE_DATA_NIBBLE     0x90   /* non-byte-aligned tag response */
#define CR95HF_CODE_DATA            0x80   /* SendRecv: tag data returned   */
#define CR95HF_CODE_TIMEOUT         0x87   /* SendRecv: no tag in the field */

#define CR95HF_PROTO_ISO14443A      0x02

/* SendRecv transmit flags (appended as the final payload byte). */
#define CR95HF_TX_SHORT_FRAME       0x07   /* 7-bit REQA/WUPA, no CRC        */
#define CR95HF_TX_NO_CRC           0x08   /* 8-bit frame, no CRC (anticol) */
#define CR95HF_TX_CRC              0x28   /* 8-bit frame + CRC-A (select/  */
                                           /*  read/write)                   */

/* ----------------------------------------------------------------- ISO14443
 * ISO14443-3A activation bytes and Type 2 tag memory commands.
 */
#define ISO14443A_WUPA              0x52
#define ISO14443A_REQA              0x26
#define ISO14443A_SEL_CL1           0x93
#define ISO14443A_SEL_CL2           0x95
#define ISO14443A_NVB_ANTICOLL      0x20
#define ISO14443A_NVB_SELECT        0x70
#define ISO14443A_CT                0x88   /* cascade tag (more UID follows) */
#define ISO14443A_ACK               0x0A   /* MIFARE UL write acknowledge    */

#define NFC_CMD_READ                0x30
#define NFC_CMD_WRITE               0xA2
#define NFC_CMD_GET_VERSION         0x60

/* ------------------------------------------------------------- NDEF / TLV
 * Type 2 tag NDEF TLV tags and the short well-known URI record layout.
 */
#define NFC_TLV_NULL                0x00
#define NFC_TLV_NDEF                0x03
#define NFC_TLV_TERMINATOR          0xFE
#define NDEF_URI_TYPE               0x55   /* 'U' well-known type */

#define NFC_FORUM_CC_MAGIC          0xE1
#define NFC_FORUM_CC_VERSION_1      0x10
#define NFC_FORUM_CC_PAGE           3
#define CR95HF_MF0UL11_CAPACITY     48
#define CR95HF_USER_PAGE            4      /* first user-data page (after CC) */
#define CR95HF_RX_BUF_SIZE          256
#define CR95HF_STATIC_LOCK0_PAGE4   0x10
#define CR95HF_STATIC_LOCK_OTP      0x08
#define CR95HF_NDEF_BUF_SIZE        256
#define CR95HF_TIMEOUT_MS           200
#define CR95HF_ECHO_ATTEMPTS        255
#define CR95HF_ECHO_TIMEOUT_MS      2
#define CR95HF_UART_RX_BUF_SIZE     2048

/* ------------------------------------------------------------ URI prefixes
 * NFC Forum RTD-URI identifier-code table. A URI record payload begins with
 * one of these codes followed by the URI remainder (DS10311 / RTD-URI 1.0).
 */
static const char *const URI_PREFIXES[] =
{
    "",                             /* 0x00 full URI stored verbatim */
    "http://www.",
    "https://www.",
    "http://",
    "https://",
    "tel:",
    "mailto:",
    "ftp://anonymous:anonymous@",
    "ftp://ftp.",
    "ftps://",
    "sftp://",
    "smb://",
    "nfs://",
    "ftp://",
    "dav://",
    "news:",
    "telnet://",
    "imap:",
    "rtsp://",
    "urn:",
    "pop:",
    "sip:",
    "sips:",
    "tftp:",
    "btspp://",
    "btl2cap://",
    "btgoep://",
    "tcpobex://",
    "irdaobex://",
    "file://",
    "urn:epc:id:",
    "urn:epc:tag:",
    "urn:epc:pat:",
    "urn:epc:raw:",
    "urn:epc:",
    "urn:nfc:",
};
#define URI_PREFIX_COUNT            (sizeof(URI_PREFIXES) / sizeof(URI_PREFIXES[0]))

/* Prefixes recognized when encoding a URL (longest match first). */
typedef struct
{
    const char *prefix;
    uint8_t     code;
} uri_prefix_map_t;

static const uri_prefix_map_t URI_PREFIX_MAP[] =
{
    { "https://www.", 0x02 },
    { "http://www.",  0x01 },
    { "https://",     0x04 },
    { "http://",      0x03 },
};
#define URI_PREFIX_MAP_COUNT        (sizeof(URI_PREFIX_MAP) / sizeof(URI_PREFIX_MAP[0]))

/* Serializes access to the UART: one command/response transaction at a time. */
static SemaphoreHandle_t s_uart_mutex = NULL;
static bool s_ready = false;

/** Look up the URI prefix string for an RTD-URI identifier code. */
static const char *cr95hf_uri_prefix(uint8_t code)
{
    if (code >= URI_PREFIX_COUNT)
    {
        return "";
    }
    return URI_PREFIXES[code];
}

/** Read exactly @p len bytes from the UART, or time out. */
static esp_err_t cr95hf_read_exact(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t total = 0;

    while (total < len)
    {
        int got = uart_read_bytes(NFC_UART_PORT, &buf[total], len - total,
                                  pdMS_TO_TICKS(timeout_ms));
        if (got <= 0)
        {
            return ESP_ERR_TIMEOUT;
        }
        total += (size_t)got;
    }
    return ESP_OK;
}

/** Format raw bytes as two-digit hex into @p dst (truncated at 64 bytes). */
static void cr95hf_format_hex(char *dst, size_t dst_cap, const uint8_t *buf,
                              size_t len)
{
    size_t off = 0;
    size_t i;

    if (len > 64)
    {
        len = 64;
    }
    for (i = 0; i < len && off + 3 < dst_cap; i++)
    {
        off += (size_t)snprintf(&dst[off], dst_cap - off, "%02x ", buf[i]);
    }
    dst[off] = '\0';
}

/**
 * Send one CR95HF command frame and read the response frame.
 *
 * @param cmd        command code
 * @param data       command payload bytes (may be NULL when data_len == 0)
 * @param data_len   payload length in bytes
 * @param code       out: response result code
 * @param rsp        out: response data bytes
 * @param rsp_len    out: response data length in bytes
 * @param timeout_ms per-chunk UART read timeout
 */
static esp_err_t cr95hf_transact(uint8_t cmd, const uint8_t *data,
                                 uint8_t data_len, uint8_t *code,
                                 uint8_t *rsp, uint8_t rsp_cap,
                                 uint8_t *rsp_len, uint32_t timeout_ms)
{
    uint8_t frame[CR95HF_RX_BUF_SIZE];
    uint8_t len = 0;
    esp_err_t err;
    int written;

    if (code == NULL || (data_len > 0 && data == NULL)
        || (size_t)data_len + 2 > sizeof(frame))
    {
        return ESP_ERR_INVALID_ARG;
    }
    *code = 0xFF;
    if (rsp_len != NULL)
    {
        *rsp_len = 0;
    }

    frame[0] = cmd;
    frame[1] = data_len;
    if (data_len > 0)
    {
        memcpy(&frame[2], data, data_len);
    }

    uart_flush_input(NFC_UART_PORT);
    written = uart_write_bytes(NFC_UART_PORT, frame, (size_t)data_len + 2);
    if (written != (int)data_len + 2)
    {
        return ESP_FAIL;
    }

    err = cr95hf_read_exact(code, 1, timeout_ms);
    if (err != ESP_OK)
    {
        return err;
    }

    err = cr95hf_read_exact(&len, 1, timeout_ms);
    if (err != ESP_OK)
    {
        return err;
    }

    if (len == 0)
    {
        if (rsp_len != NULL)
        {
            *rsp_len = 0;
        }
        return ESP_OK;
    }


    err = cr95hf_read_exact(frame, len, timeout_ms);
    if (err != ESP_OK)
    {
        return err;
    }
    if (rsp == NULL || len > rsp_cap)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(rsp, frame, len);

    if (rsp_len != NULL)
    {
        *rsp_len = len;
    }
    return ESP_OK;
}
/**
 * Synchronize the UART transport using the CR95HF's special Echo frame.
 *
 * A non-Echo reply means the two ends were out of framing; discard that
 * response before trying Echo again. Keep below the part's 528-Echo FIFO
 * recovery limit.
 */
static esp_err_t cr95hf_echo_sync(void)
{
    static const uint8_t command[] = { 0x00, CR95HF_CMD_ECHO };
    uint8_t response;

    uart_flush_input(NFC_UART_PORT);
    for (int attempt = 0; attempt < CR95HF_ECHO_ATTEMPTS; attempt++)
    {
        int received;

        if (uart_write_bytes(NFC_UART_PORT, command, sizeof(command))
            != (int)sizeof(command))
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        received = uart_read_bytes(NFC_UART_PORT, &response, 1,
                                   pdMS_TO_TICKS(CR95HF_ECHO_TIMEOUT_MS));
        if (received == 1 && response == CR95HF_CMD_ECHO)
        {
            ESP_LOGI(TAG, "CR95HF synchronized after %d echo attempt(s)",
                     attempt + 1);
            return ESP_OK;
        }
        if (received == 1)
        {
            /* Drain the mismatched CR95HF reply through an RX idle period. */
            while (uart_read_bytes(NFC_UART_PORT, &response, 1,
                                   pdMS_TO_TICKS(CR95HF_ECHO_TIMEOUT_MS)) > 0)
            {
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return ESP_ERR_TIMEOUT;
}


/** SendRecv: exchange raw RF bytes with the tag using the given TX flags. */
static esp_err_t cr95hf_send_recv(const uint8_t *rf, uint8_t rf_len,
                                  uint8_t flags, uint8_t *code,
                                  uint8_t *rsp, uint8_t rsp_cap,
                                  uint8_t *rsp_len)
{
    uint8_t data[CR95HF_RX_BUF_SIZE];

    if ((size_t)rf_len + 1 > sizeof(data))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(data, rf, rf_len);
    data[rf_len] = flags;

    return cr95hf_transact(CR95HF_CMD_SENDRECV, data, (uint8_t)(rf_len + 1),
                           code, rsp, rsp_cap, rsp_len, CR95HF_TIMEOUT_MS);
}

/**
 * Validate a complete normal-framing ISO14443-A response. The CR95HF appends
 * two received CRC-A bytes when CRC reception is enabled, then three status
 * bytes. A no-CRC exchange reports the expected CRC-missing bit in status.
 */
static bool cr95hf_type_a_response(const char *operation, uint8_t code,
                                   const uint8_t *rsp, uint8_t rsp_len,
                                   size_t data_len, bool with_crc)
{
    size_t status_offset = data_len + (with_crc ? 2 : 0);
    size_t expected_len = status_offset + 3;
    uint8_t expected_status = with_crc ? 0x08 : 0x28;

    if (code != CR95HF_CODE_DATA || rsp_len != expected_len
        || rsp[status_offset] != expected_status
        || rsp[status_offset + 1] != 0
        || rsp[status_offset + 2] != 0)
    {
        char raw[3 * 64 + 1];
        cr95hf_format_hex(raw, sizeof(raw), rsp, rsp_len);
        ESP_LOGW(TAG,
                 "%s invalid response: code=0x%02x len=%u raw=%s",
                 operation, code, (unsigned)rsp_len, raw);
        if (rsp_len >= 3)
        {
            size_t tag_bytes = rsp_len - 3;
            uint8_t status = rsp[tag_bytes];
            uint8_t collision_byte = rsp[tag_bytes + 1];
            uint8_t collision_bit = rsp[tag_bytes + 2];

            ESP_LOGW(TAG,
                     "%s RF detail: tag_bytes=%u status=0x%02x"
                     " significant_bits=%u collision_byte=%u collision_bit=%u",
                     operation, (unsigned)tag_bytes, status,
                     (unsigned)(status & 0x0f), collision_byte, collision_bit);
        }
        return false;
    }
    return true;
}

/** Select an RF protocol with the given parameters. */
static esp_err_t cr95hf_protocol_select(const uint8_t *params, uint8_t len)
{
    uint8_t code = 0xFF;
    uint8_t rsp[8];
    uint8_t rsp_len = 0;
    esp_err_t err;

    err = cr95hf_transact(CR95HF_CMD_PROTOCOL_SELECT, params, len, &code,
                          rsp, sizeof(rsp), &rsp_len, CR95HF_TIMEOUT_MS);
    if (err != ESP_OK || code != CR95HF_CODE_SUCCESS || rsp_len != 0)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/** Select ISO14443-A with the recovered stock startup parameters. */
static esp_err_t cr95hf_select_iso14443a(void)
{
    static const uint8_t params[2] = { 0x02, 0x00 };
    return cr95hf_protocol_select(params, sizeof(params));
}

/**
 * Turn the RF field off between tag hunts. A tag left in the ACTIVE state is
 * power-cycled back to IDLE, so the next activation starts from a clean state.
 */
static void cr95hf_field_off(void)
{
    static const uint8_t off[2] = { 0x00, 0x00 };
    cr95hf_protocol_select(off, sizeof(off));
}

/**
 * Type-A receiver tuning for the Yoto antenna. TimerW 0x58 synchronizes the
 * analog/digital receive paths; ARC_B D3 keeps 95% modulation with 27 dB RX
 * gain, improving marginal clone load modulation without D0 saturation.
 */
static bool cr95hf_configure_rf(void)
{
    static const uint8_t timer_w[] = { 0x3A, 0x00, 0x58, 0x04 };
    static const uint8_t modulation_gain[] = { 0x68, 0x01, 0x01, 0xD3 };
    uint8_t code = 0xFF;
    uint8_t rsp[4];
    uint8_t rsp_len;
    esp_err_t err;

    err = cr95hf_transact(CR95HF_CMD_WRREG, timer_w, sizeof(timer_w),
                          &code, rsp, sizeof(rsp), &rsp_len,
                          CR95HF_TIMEOUT_MS);
    if (err != ESP_OK || code != CR95HF_CODE_SUCCESS)
    {
        ESP_LOGW(TAG, "CR95HF TimerW configuration failed: err=%s code=0x%02x",
                 esp_err_to_name(err), code);
        return false;
    }

    err = cr95hf_transact(CR95HF_CMD_WRREG, modulation_gain,
                          sizeof(modulation_gain), &code, rsp, sizeof(rsp),
                          &rsp_len, CR95HF_TIMEOUT_MS);
    if (err != ESP_OK || code != CR95HF_CODE_SUCCESS)
    {
        ESP_LOGW(TAG,
                 "CR95HF modulation/gain configuration failed: err=%s code=0x%02x",
                 esp_err_to_name(err), code);
        return false;
    }
    return true;
}


/**
 * Activate a Type A tag and return its UID (4 or 7 bytes). Runs REQA,
 * anticollision and select (cascade levels 1 and 2). Returns false when no
 * tag is in the field.
 */
static bool cr95hf_activate(uint8_t *uid, uint8_t *uid_len, uint8_t *sak)
{
    uint8_t reqa = ISO14443A_REQA;
    uint8_t wupa = ISO14443A_WUPA;
    uint8_t anticol1[2] = { ISO14443A_SEL_CL1, ISO14443A_NVB_ANTICOLL };
    uint8_t anticol2[2] = { ISO14443A_SEL_CL2, ISO14443A_NVB_ANTICOLL };
    uint8_t select1[7];
    uint8_t select2[7];
    uint8_t cl1[5];
    uint8_t cl2[5];
    uint8_t code = 0xFF;
    uint8_t rsp[24];
    uint8_t rsp_len = 0;
    esp_err_t err;
    bool request_ok;

    if (cr95hf_select_iso14443a() != ESP_OK)
    {
        ESP_LOGW(TAG, "ProtocolSelect (ISO14443A) failed before activation");
        return false;
    }
    if (!cr95hf_configure_rf())
    {
        return false;
    }

    err = cr95hf_send_recv(&reqa, 1, CR95HF_TX_SHORT_FRAME, &code, rsp,
                           sizeof(rsp), &rsp_len);
    request_ok = err == ESP_OK && code == CR95HF_CODE_DATA
              && cr95hf_type_a_response("REQA", code, rsp, rsp_len, 2, false);
    if (!request_ok)
    {
        /*
         * REQA addresses IDLE tags only. WUPA also recovers a tag left in
         * HALT by an earlier transaction.
         */
        err = cr95hf_send_recv(&wupa, 1, CR95HF_TX_SHORT_FRAME, &code, rsp,
                               sizeof(rsp), &rsp_len);
        request_ok = err == ESP_OK && code == CR95HF_CODE_DATA
                  && cr95hf_type_a_response("WUPA", code, rsp, rsp_len, 2,
                                            false);
        if (!request_ok)
        {
            /* Frame-wait timeout is the normal no-card result; keep the
             * 100-ms poll loop quiet unless transport itself misbehaves. */
            if (err != ESP_OK || code != CR95HF_CODE_TIMEOUT)
            {
                ESP_LOGW(TAG,
                         "Type A activation failed: REQA/WUPA transport=%s"
                         " code=0x%02x len=%u",
                         esp_err_to_name(err), code, (unsigned)rsp_len);
            }
            return false;
        }
    }

    err = cr95hf_send_recv(anticol1, 2, CR95HF_TX_NO_CRC, &code, rsp,
                           sizeof(rsp), &rsp_len);
    if (err != ESP_OK
        || !cr95hf_type_a_response("anticollision L1", code, rsp, rsp_len,
                                   5, false))
    {
        ESP_LOGW(TAG,
                 "anticollision L1 failed: transport=%s code=0x%02x len=%u",
                 esp_err_to_name(err), code, (unsigned)rsp_len);
        return false;
    }
    memcpy(cl1, rsp, 5);
    if ((uint8_t)(cl1[0] ^ cl1[1] ^ cl1[2] ^ cl1[3]) != cl1[4])
    {
        ESP_LOGW(TAG, "anticollision L1 BCC mismatch");
        return false;
    }

    select1[0] = ISO14443A_SEL_CL1;
    select1[1] = ISO14443A_NVB_SELECT;
    memcpy(&select1[2], cl1, 5);
    err = cr95hf_send_recv(select1, 7, CR95HF_TX_CRC, &code, rsp,
                           sizeof(rsp), &rsp_len);
    if (err != ESP_OK
        || !cr95hf_type_a_response("select L1", code, rsp, rsp_len, 1, true))
    {
        ESP_LOGW(TAG, "select L1 failed: transport=%s code=0x%02x len=%u",
                 esp_err_to_name(err), code, (unsigned)rsp_len);
        return false;
    }

    if (cl1[0] != ISO14443A_CT)
    {
        if ((rsp[0] & 0x04) != 0 || *uid_len < 4)
        {
            return false;
        }
        if (sak != NULL)
        {
            *sak = rsp[0];
        }
        memcpy(uid, cl1, 4);
        *uid_len = 4;
        return true;
    }
    if ((rsp[0] & 0x04) == 0)
    {
        ESP_LOGW(TAG, "select L1 did not advertise UID cascade");
        return false;
    }

    /* Cascade tag 0x88: a 7-byte UID — run cascade level 2. */
    err = cr95hf_send_recv(anticol2, 2, CR95HF_TX_NO_CRC, &code, rsp,
                           sizeof(rsp), &rsp_len);
    if (err != ESP_OK
        || !cr95hf_type_a_response("anticollision L2", code, rsp, rsp_len,
                                   5, false))
    {
        ESP_LOGW(TAG,
                 "anticollision L2 failed: transport=%s code=0x%02x len=%u",
                 esp_err_to_name(err), code, (unsigned)rsp_len);
        return false;
    }
    memcpy(cl2, rsp, 5);
    if ((uint8_t)(cl2[0] ^ cl2[1] ^ cl2[2] ^ cl2[3]) != cl2[4])
    {
        ESP_LOGW(TAG, "anticollision L2 BCC mismatch");
        return false;
    }

    select2[0] = ISO14443A_SEL_CL2;
    select2[1] = ISO14443A_NVB_SELECT;
    memcpy(&select2[2], cl2, 5);
    err = cr95hf_send_recv(select2, 7, CR95HF_TX_CRC, &code, rsp,
                           sizeof(rsp), &rsp_len);
    if (err != ESP_OK
        || !cr95hf_type_a_response("select L2", code, rsp, rsp_len, 1, true))
    {
        ESP_LOGW(TAG, "select L2 failed: transport=%s code=0x%02x len=%u",
                 esp_err_to_name(err), code, (unsigned)rsp_len);
        return false;
    }
    if ((rsp[0] & 0x04) != 0 || cl2[0] == ISO14443A_CT || *uid_len < 7)
    {
        ESP_LOGW(TAG, "unsupported UID cascade after level 2");
        return false;
    }
    if (sak != NULL)
    {
        *sak = rsp[0];
    }

    uid[0] = cl1[1];
    uid[1] = cl1[2];
    uid[2] = cl1[3];
    memcpy(&uid[3], cl2, 4);
    *uid_len = 7;
    return true;
}

/** Read 4 pages (16 bytes) starting at @p page into @p out16. */
static esp_err_t cr95hf_read_pages(uint8_t page, uint8_t *out16)
{
    uint8_t rf[2] = { NFC_CMD_READ, page };
    uint8_t code = 0xFF;
    uint8_t rsp[24];
    uint8_t rsp_len = 0;
    esp_err_t err;

    err = cr95hf_send_recv(rf, 2, CR95HF_TX_CRC, &code, rsp, sizeof(rsp),
                           &rsp_len);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Type 2 READ page %u transport failed: %s",
                 (unsigned)page, esp_err_to_name(err));
        return err;
    }
    if (!cr95hf_type_a_response("Type 2 READ", code, rsp, rsp_len, 16, true))
    {
        ESP_LOGW(TAG,
                 "Type 2 READ page %u rejected: code=0x%02x len=%u"
                 " rsp=%02x %02x",
                 (unsigned)page, code, (unsigned)rsp_len,
                 rsp_len > 0 ? rsp[0] : 0, rsp_len > 1 ? rsp[1] : 0);
        return ESP_FAIL;
    }
    memcpy(out16, rsp, 16);
    return ESP_OK;
}

/** Read MIFARE Ultralight static lock bytes from page 2 bytes 2 and 3. */
static esp_err_t cr95hf_read_static_locks(uint8_t *lock0, uint8_t *lock1)
{
    uint8_t pages[16];
    esp_err_t err = cr95hf_read_pages(0, pages);

    if (err != ESP_OK)
    {
        return err;
    }
    *lock0 = pages[10];
    *lock1 = pages[11];
    return ESP_OK;
}

/** Return true when a static lock bit makes a first-16-memory page read-only. */
static bool cr95hf_page_is_locked(uint8_t page, uint8_t lock0, uint8_t lock1)
{
    if (page >= 4 && page <= 7)
    {
        return (lock0 & (uint8_t)(CR95HF_STATIC_LOCK0_PAGE4 << (page - 4))) != 0;
    }
    if (page >= 8 && page <= 15)
    {
        return (lock1 & (uint8_t)(1U << (page - 8))) != 0;
    }
    return false;
}

/**
 * Walk an NDEF message and decode the first well-known URI record. Supports
 * short and normal records, optional IDs, and messages where another record
 * precedes the URI.
 */
static bool cr95hf_decode_uri(const uint8_t *message, size_t message_len,
                              char *url, size_t url_cap)
{
    size_t offset = 0;

    while (offset < message_len)
    {
        uint8_t header = message[offset++];
        uint8_t tnf = header & 0x07;
        bool short_record = (header & 0x10) != 0;
        bool has_id = (header & 0x08) != 0;
        bool message_end = (header & 0x40) != 0;
        uint8_t type_len;
        uint8_t id_len = 0;
        uint32_t payload_len;
        const uint8_t *type;
        const uint8_t *payload;

        if ((header & 0x20) != 0 || offset >= message_len)
        {
            return false; /* chunked records are not supported */
        }
        type_len = message[offset++];
        if (short_record)
        {
            if (offset >= message_len)
            {
                return false;
            }
            payload_len = message[offset++];
        }
        else
        {
            if (message_len - offset < 4)
            {
                return false;
            }
            payload_len = ((uint32_t)message[offset] << 24)
                        | ((uint32_t)message[offset + 1] << 16)
                        | ((uint32_t)message[offset + 2] << 8)
                        | message[offset + 3];
            offset += 4;
        }
        if (has_id)
        {
            if (offset >= message_len)
            {
                return false;
            }
            id_len = message[offset++];
        }
        if ((uint64_t)offset + type_len + id_len + payload_len > message_len)
        {
            return false;
        }
        type = &message[offset];
        offset += type_len + id_len;
        payload = &message[offset];
        offset += payload_len;

        if (tnf == 0x01 && type_len == 1 && type[0] == NDEF_URI_TYPE
            && payload_len >= 1)
        {
            const char *prefix = cr95hf_uri_prefix(payload[0]);
            size_t prefix_len = strlen(prefix);
            size_t uri_len = payload_len - 1;

            if (url == NULL || url_cap == 0)
            {
                return true;
            }
            if (prefix_len + uri_len + 1 > url_cap)
            {
                url[0] = '\0';
                return false;
            }
            memcpy(url, prefix, prefix_len);
            memcpy(url + prefix_len, payload + 1, uri_len);
            url[prefix_len + uri_len] = '\0';
            return true;
        }
        if (message_end)
        {
            break;
        }
    }
    return false;
}

typedef enum
{
    CR95HF_NDEF_NEED_MORE,
    CR95HF_NDEF_FOUND_URI,
    CR95HF_NDEF_EMPTY,
    CR95HF_NDEF_DONE_NO_URI,
    CR95HF_NDEF_MALFORMED,
} cr95hf_ndef_scan_t;

/**
 * Scan the bytes read so far. A complete TLV is decoded immediately so a
 * short NDEF record does not depend on later, unrelated user-memory reads.
 */
static cr95hf_ndef_scan_t cr95hf_scan_ndef(const uint8_t *buf, size_t len,
                                           bool complete, char *url,
                                           size_t url_cap)
{
    size_t i = 0;

    while (i < len)
    {
        uint8_t tlv = buf[i];
        size_t header_len;
        size_t value_len;

        if (tlv == NFC_TLV_NULL)
        {
            i++;
            continue;
        }
        if (tlv == NFC_TLV_TERMINATOR)
        {
            return CR95HF_NDEF_EMPTY;
        }
        if (len - i < 2)
        {
            return complete ? CR95HF_NDEF_MALFORMED : CR95HF_NDEF_NEED_MORE;
        }

        if (buf[i + 1] == 0xFF)
        {
            if (len - i < 4)
            {
                return complete ? CR95HF_NDEF_MALFORMED
                                : CR95HF_NDEF_NEED_MORE;
            }
            value_len = ((size_t)buf[i + 2] << 8) | buf[i + 3];
            header_len = 4;
        }
        else
        {
            value_len = buf[i + 1];
            header_len = 2;
        }

        if (value_len > len - i - header_len)
        {
            return complete ? CR95HF_NDEF_MALFORMED : CR95HF_NDEF_NEED_MORE;
        }
        if (tlv == NFC_TLV_NDEF)
        {
            if (value_len == 0)
            {
                return CR95HF_NDEF_EMPTY;
            }
            return cr95hf_decode_uri(&buf[i + header_len], value_len,
                                     url, url_cap)
                 ? CR95HF_NDEF_FOUND_URI : CR95HF_NDEF_DONE_NO_URI;
        }
        i += header_len + value_len;
    }

    return complete ? CR95HF_NDEF_EMPTY : CR95HF_NDEF_NEED_MORE;
}

/**
 * Read and validate the NFC Forum Type 2 Capability Container. Byte 2 gives
 * the user-data area in eight-byte units; the stock firmware reads this page
 * before walking the NDEF area instead of probing past the end of the tag.
 */
static bool cr95hf_read_cc(size_t *capacity, bool *writable, bool *read_ok,
                           uint8_t cc[4])
{
    uint8_t pages[16];

    if (read_ok != NULL)
    {
        *read_ok = false;
    }
    if (cr95hf_read_pages(NFC_FORUM_CC_PAGE, pages) != ESP_OK)
    {
        ESP_LOGW(TAG, "NDEF capability-container read failed");
        return false;
    }
    if (read_ok != NULL)
    {
        *read_ok = true;
    }
    if (cc != NULL)
    {
        memcpy(cc, pages, 4);
    }
    if (pages[0] != NFC_FORUM_CC_MAGIC
        || (pages[1] & 0xF0) != NFC_FORUM_CC_VERSION_1
        || pages[2] == 0)
    {
        if (pages[0] != 0 || pages[1] != 0 || pages[2] != 0 || pages[3] != 0)
        {
            ESP_LOGW(TAG,
                     "invalid Type 2 capability container: %02x %02x %02x %02x",
                     pages[0], pages[1], pages[2], pages[3]);
        }
        return false;
    }

    *capacity = (size_t)pages[2] * 8;
    if (*capacity > CR95HF_NDEF_BUF_SIZE)
    {
        *capacity = CR95HF_NDEF_BUF_SIZE;
    }
    if (writable != NULL)
    {
        *writable = (pages[3] & 0x0F) == 0;
    }
    return true;
}

/**
 * Read the Type-2 NDEF area, retaining enough state to distinguish an empty
 * tag from a card that must never be overwritten automatically.
 */
static cr95hf_card_state_t cr95hf_read_card(char *url, size_t url_cap,
                                            cr95hf_card_info_t *info)
{
    uint8_t buf[CR95HF_NDEF_BUF_SIZE];
    uint8_t cc[4] = { 0 };
    size_t capacity = 0;
    size_t used = 0;
    uint8_t page = CR95HF_USER_PAGE;
    bool writable = false;
    bool read_ok = false;
    cr95hf_card_state_t state = CR95HF_CARD_UNREADABLE;

    if (!cr95hf_read_cc(&capacity, &writable, &read_ok, cc))
    {
        if (info != NULL)
        {
            memcpy(info->cc, cc, sizeof(cc));
        }
        return read_ok && memcmp(cc, (const uint8_t[4]){ 0 }, sizeof(cc)) == 0
             ? CR95HF_CARD_BLANK : CR95HF_CARD_UNREADABLE;
    }

    if (info != NULL)
    {
        memcpy(info->cc, cc, sizeof(cc));
        info->capacity = capacity;
        info->writable = writable;
    }

    while (used < capacity)
    {
        uint8_t pages[16];
        size_t copy_len = capacity - used;
        cr95hf_ndef_scan_t scan;

        if (cr95hf_read_pages(page, pages) != ESP_OK)
        {
            ESP_LOGW(TAG, "NDEF user-area read failed at page %u",
                     (unsigned)page);
            return CR95HF_CARD_UNREADABLE;
        }
        if (copy_len > sizeof(pages))
        {
            copy_len = sizeof(pages);
        }
        memcpy(&buf[used], pages, copy_len);
        if (info != NULL)
        {
            memcpy(&info->raw_ndef[used], pages, copy_len);
            info->raw_ndef_len = used + copy_len;
        }
        used += copy_len;

        if (state == CR95HF_CARD_UNREADABLE)
        {
            scan = cr95hf_scan_ndef(buf, used, used == capacity, url, url_cap);
            if (scan == CR95HF_NDEF_FOUND_URI)
            {
                state = CR95HF_CARD_URI;
            }
            else if (scan == CR95HF_NDEF_EMPTY)
            {
                state = writable ? CR95HF_CARD_BLANK : CR95HF_CARD_NON_URI;
            }
            else if (scan == CR95HF_NDEF_DONE_NO_URI
                     || scan == CR95HF_NDEF_MALFORMED)
            {
                state = CR95HF_CARD_NON_URI;
            }
            if (state != CR95HF_CARD_UNREADABLE && info == NULL)
            {
                return state;
            }
        }
        page = (uint8_t)(page + 4);
    }
    return state == CR95HF_CARD_UNREADABLE ? CR95HF_CARD_NON_URI : state;
}

/** Read and decode the first URI, preserving the historical boolean API. */
static bool cr95hf_read_url(char *url, size_t url_cap)
{
    return cr95hf_read_card(url, url_cap, NULL) == CR95HF_CARD_URI;
}

/**
 * Split @p url into a URI prefix code and a remainder, returning the code and
 * advancing @p remainder past the matched prefix (or to the start of @p url
 * when no prefix matches).
 */
static uint8_t cr95hf_encode_uri(const char *url, const char **remainder)
{
    size_t i;

    for (i = 0; i < URI_PREFIX_MAP_COUNT; i++)
    {
        size_t plen = strlen(URI_PREFIX_MAP[i].prefix);

        if (strncmp(url, URI_PREFIX_MAP[i].prefix, plen) == 0)
        {
            *remainder = url + plen;
            return URI_PREFIX_MAP[i].code;
        }
    }

    *remainder = url;
    return 0x00;                    /* no prefix match: store the full URI */
}

/** Positively identify the 48-byte MIFARE Ultralight EV1 before OTP writes. */
static bool cr95hf_is_mf0ul11(void)
{
    static const uint8_t identity[] = { 0x00, 0x04, 0x03 };
    uint8_t command = NFC_CMD_GET_VERSION;
    uint8_t code = 0xFF;
    uint8_t rsp[16];
    uint8_t rsp_len = 0;
    esp_err_t err;

    err = cr95hf_send_recv(&command, 1, CR95HF_TX_CRC, &code, rsp,
                           sizeof(rsp), &rsp_len);
    if (err != ESP_OK
        || !cr95hf_type_a_response("GET_VERSION", code, rsp, rsp_len, 8, true))
    {
        return false;
    }

    return memcmp(rsp, identity, sizeof(identity)) == 0
        && (rsp[3] == 0x01 || rsp[3] == 0x02)
        && rsp[4] == 0x01
        && rsp[5] == 0x00
        && rsp[6] == 0x0B
        && rsp[7] == 0x03;
}

/**
 * Write one Type 2 page and validate the CR95HF's four-bit ACK response.
 *
 * On a timeout, the page might still have been programmed. Verify it without
 * changing the selected tag first; only then cycle the RF field and require
 * the same UID before a recovery read.
 */
static esp_err_t cr95hf_write_page(uint8_t page, const uint8_t data[4],
                                   const uint8_t *expected_uid,
                                   uint8_t expected_uid_len)
{
    uint8_t rf[6] = {
        NFC_CMD_WRITE, page, data[0], data[1], data[2], data[3],
    };
    uint8_t code = 0xFF;
    uint8_t rsp[4];
    uint8_t rsp_len = 0;
    esp_err_t err;

    ESP_LOGI(TAG, "Type 2 WRITE page %u: A2 %02x %02x %02x %02x",
             (unsigned)page, data[0], data[1], data[2], data[3]);
    err = cr95hf_send_recv(rf, sizeof(rf), CR95HF_TX_CRC, &code, rsp,
                           sizeof(rsp), &rsp_len);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Type 2 page %u transport write failed: %s",
                 (unsigned)page, esp_err_to_name(err));
        return err;
    }
    {
        char raw[3 * sizeof(rsp) + 1];

        cr95hf_format_hex(raw, sizeof(raw), rsp, rsp_len);
        ESP_LOGI(TAG, "Type 2 WRITE page %u response: code=0x%02x len=%u raw=%s",
                 (unsigned)page, code, (unsigned)rsp_len, raw);
    }
    if (code == CR95HF_CODE_DATA_NIBBLE && rsp_len == 4
        && rsp[0] == ISO14443A_ACK
        && rsp[1] == 0x24 && rsp[2] == 0 && rsp[3] == 0)
    {
        vTaskDelay(pdMS_TO_TICKS(6));
        return ESP_OK;
    }

    /*
     * ST's Type-2 example receives 87 00 after A2, then proves the write
     * using READ. Do not reactivate before that read: doing so changes the
     * tag state before observing the command's result.
     */
    if (code == CR95HF_CODE_TIMEOUT && rsp_len == 0)
    {
        uint8_t verify[16];
        uint8_t uid[CR95HF_UID_MAX];
        uint8_t uid_len = sizeof(uid);

        ESP_LOGI(TAG,
                 "Type 2 WRITE page %u timed out; waiting 6 ms for EEPROM then reading selected tag",
                 (unsigned)page);
        vTaskDelay(pdMS_TO_TICKS(6));
        err = cr95hf_read_pages(page, verify);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG,
                     "Type 2 WRITE page %u same-session readback: got=%02x %02x %02x %02x expected=%02x %02x %02x %02x",
                     (unsigned)page, verify[0], verify[1], verify[2], verify[3],
                     data[0], data[1], data[2], data[3]);
            if (memcmp(verify, data, 4) == 0)
            {
                ESP_LOGI(TAG, "Type 2 page %u write verified after timeout",
                         (unsigned)page);
                return ESP_OK;
            }
        }
        else
        {
            ESP_LOGW(TAG,
                     "Type 2 WRITE page %u same-session read failed: %s",
                     (unsigned)page, esp_err_to_name(err));
        }

        ESP_LOGI(TAG, "Type 2 WRITE page %u starting RF recovery",
                 (unsigned)page);
        cr95hf_field_off();
        vTaskDelay(pdMS_TO_TICKS(5));
        if (!cr95hf_activate(uid, &uid_len, NULL))
        {
            ESP_LOGE(TAG, "Type 2 WRITE page %u recovery activation failed",
                     (unsigned)page);
            return ESP_FAIL;
        }
        if (uid_len != expected_uid_len
            || memcmp(uid, expected_uid, uid_len) != 0)
        {
            char actual_uid[3 * CR95HF_UID_MAX + 1];
            char wanted_uid[3 * CR95HF_UID_MAX + 1];

            cr95hf_format_hex(actual_uid, sizeof(actual_uid), uid, uid_len);
            cr95hf_format_hex(wanted_uid, sizeof(wanted_uid), expected_uid,
                              expected_uid_len);
            ESP_LOGE(TAG,
                     "Type 2 WRITE page %u recovery UID changed: got=%s expected=%s",
                     (unsigned)page, actual_uid, wanted_uid);
            return ESP_ERR_INVALID_STATE;
        }
        err = cr95hf_read_pages(page, verify);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG,
                     "Type 2 WRITE page %u recovered readback: got=%02x %02x %02x %02x expected=%02x %02x %02x %02x",
                     (unsigned)page, verify[0], verify[1], verify[2], verify[3],
                     data[0], data[1], data[2], data[3]);
            if (memcmp(verify, data, 4) == 0)
            {
                ESP_LOGI(TAG, "Type 2 page %u write verified after RF recovery",
                         (unsigned)page);
                return ESP_OK;
            }
        }
        else
        {
            ESP_LOGW(TAG, "Type 2 WRITE page %u recovered read failed: %s",
                     (unsigned)page, esp_err_to_name(err));
        }
        ESP_LOGE(TAG, "Type 2 page %u timeout and readback mismatch",
                 (unsigned)page);
        return ESP_FAIL;
    }

    ESP_LOGE(TAG,
             "Type 2 page %u write rejected: code=0x%02x len=%u rsp=%02x %02x",
             (unsigned)page, code, (unsigned)rsp_len,
             rsp_len > 0 ? rsp[0] : 0, rsp_len > 1 ? rsp[1] : 0);
    return ESP_FAIL;
}

/**
 * Format an all-zero MF0UL11 CC safely. Page 4 advertises an empty NDEF
 * message before the irreversible OTP/CC bits make the tag discoverable.
 */
static esp_err_t cr95hf_format_blank_mf0ul11(const uint8_t cc[4],
                                             uint8_t lock0, uint8_t lock1,
                                             const uint8_t *expected_uid,
                                             uint8_t expected_uid_len)
{
    static const uint8_t zero_cc[4] = { 0, 0, 0, 0 };
    static const uint8_t empty_ndef[4] = {
        NFC_TLV_NDEF, 0x00, NFC_TLV_TERMINATOR, 0x00,
    };
    static const uint8_t formatted_cc[4] = {
        NFC_FORUM_CC_MAGIC, NFC_FORUM_CC_VERSION_1, 0x06, 0x00,
    };
    uint8_t verify[16];
    esp_err_t err;

    if (memcmp(cc, zero_cc, sizeof(zero_cc)) != 0)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((lock0 & CR95HF_STATIC_LOCK_OTP) != 0
        || cr95hf_page_is_locked(CR95HF_USER_PAGE, lock0, lock1))
    {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (!cr95hf_is_mf0ul11())
    {
        ESP_LOGE(TAG, "blank CC belongs to an unsupported Type 2 tag");
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = cr95hf_write_page(CR95HF_USER_PAGE, empty_ndef, expected_uid,
                            expected_uid_len);
    if (err != ESP_OK)
    {
        return err;
    }
    err = cr95hf_write_page(NFC_FORUM_CC_PAGE, formatted_cc, expected_uid,
                            expected_uid_len);
    if (err != ESP_OK)
    {
        return err;
    }
    err = cr95hf_read_pages(NFC_FORUM_CC_PAGE, verify);
    if (err != ESP_OK || memcmp(verify, formatted_cc, 4) != 0)
    {
        ESP_LOGE(TAG, "MF0UL11 capability-container verification failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "formatted blank MF0UL11 capability container");
    return ESP_OK;
}


/** Build the NDEF URI record + TLV and write it page by page from page 4. */
static esp_err_t cr95hf_write_ndef(const char *url, size_t capacity,
                                   uint8_t lock0, uint8_t lock1,
                                   const uint8_t *expected_uid,
                                   uint8_t expected_uid_len)
{
    uint8_t record[CR95HF_NDEF_BUF_SIZE];
    uint8_t tlv[CR95HF_NDEF_BUF_SIZE + 8];
    const char *remainder = NULL;
    uint8_t prefix;
    size_t uri_len;
    size_t payload_len;
    size_t record_len;
    size_t tlv_len;
    size_t off;
    esp_err_t err;

    prefix = cr95hf_encode_uri(url, &remainder);
    uri_len = strlen(remainder);

    if (uri_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (uri_len > CR95HF_URL_MAX)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    payload_len = 1 + uri_len;      /* prefix code + URI bytes */
    if (payload_len > 0xFF)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    /* NDEF short well-known URI record: D1 01 <plen> 'U' <prefix> <uri>. */
    record[0] = 0xD1;
    record[1] = 0x01;
    record[2] = (uint8_t)payload_len;
    record[3] = NDEF_URI_TYPE;
    record[4] = prefix;
    memcpy(&record[5], remainder, uri_len);
    record_len = 5 + uri_len;

    /* Wrap in an NDEF-message TLV, terminate, and pad to whole 4-byte pages. */
    tlv[0] = NFC_TLV_NDEF;
    tlv[1] = (uint8_t)record_len;
    memcpy(&tlv[2], record, record_len);
    tlv_len = 2 + record_len;
    tlv[tlv_len++] = NFC_TLV_TERMINATOR;
    while ((tlv_len % 4) != 0)
    {
        tlv[tlv_len++] = 0x00;
    }
    if (tlv_len > capacity)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Reject every locked destination before modifying any page. */
    for (off = 0; off < tlv_len; off += 4)
    {
        uint8_t page = (uint8_t)(CR95HF_USER_PAGE + (off / 4));

        if (cr95hf_page_is_locked(page, lock0, lock1))
        {
            ESP_LOGE(TAG, "Type 2 page %u is statically locked",
                     (unsigned)page);
            return ESP_ERR_NOT_ALLOWED;
        }
    }

    /*
     * Make interrupted writes read as an empty NDEF message. Publish the real
     * length only after all continuation pages have been verified.
     */
    uint8_t staged_first_page[4];
    memcpy(staged_first_page, tlv, sizeof(staged_first_page));
    staged_first_page[1] = 0;
    err = cr95hf_write_page(CR95HF_USER_PAGE, staged_first_page, expected_uid,
                            expected_uid_len);
    if (err != ESP_OK)
    {
        return err;
    }
    for (off = 4; off < tlv_len; off += 4)
    {
        uint8_t page = (uint8_t)(CR95HF_USER_PAGE + (off / 4));

        err = cr95hf_write_page(page, &tlv[off], expected_uid,
                                expected_uid_len);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    err = cr95hf_write_page(CR95HF_USER_PAGE, tlv, expected_uid,
                            expected_uid_len);
    if (err != ESP_OK)
    {
        return err;
    }
    ESP_LOGI(TAG, "NDEF write sent %u bytes across %u page(s)",
             (unsigned)tlv_len, (unsigned)(tlv_len / 4));

    return ESP_OK;
}

/* ------------------------------------------------------------------- init */

esp_err_t cr95hf_init(void)
{
    uart_config_t cfg =
    {
        .baud_rate  = NFC_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_2,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uint8_t code = 0xFF;
    uint8_t rsp[16];
    char idn[17];
    uint8_t rsp_len = 0;
    esp_err_t err;
    bool driver_installed = false;

    if (s_uart_mutex == NULL)
    {
        s_uart_mutex = xSemaphoreCreateMutex();
        if (s_uart_mutex == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    if (s_ready)
    {
        xSemaphoreGive(s_uart_mutex);
        return ESP_OK;
    }

    err = uart_driver_install(NFC_UART_PORT, CR95HF_UART_RX_BUF_SIZE,
                              0, 0, NULL, 0);
    if (err != ESP_OK)
    {
        goto done;
    }
    driver_installed = true;

    err = uart_param_config(NFC_UART_PORT, &cfg);
    if (err != ESP_OK)
    {
        goto done;
    }

    /*
     * RX/IRQ_IN is the reader's input on GPIO32 (the ESP TX line). Pulse it
     * before attaching the UART, then allow the CR95HF oscillator to start.
     */
    err = gpio_set_direction(PIN_NFC_TX, GPIO_MODE_OUTPUT);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = gpio_set_level(PIN_NFC_TX, 1);
    if (err != ESP_OK)
    {
        goto done;
    }
    /*
     * IRQ_IN needs a >=10-us low wake pulse. Hold GPIO32 high first, then
     * low for 1 ms to tolerate the board level shifter, then allow the
     * 10-ms-max HFO setup interval before sending Echo.
     */
    esp_rom_delay_us(1000);
    err = gpio_set_level(PIN_NFC_TX, 0);
    if (err != ESP_OK)
    {
        goto done;
    }
    esp_rom_delay_us(1000);
    err = gpio_set_level(PIN_NFC_TX, 1);
    if (err != ESP_OK)
    {
        goto done;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    err = gpio_set_direction(PIN_NFC_RX, GPIO_MODE_INPUT);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = uart_set_pin(NFC_UART_PORT, PIN_NFC_TX, PIN_NFC_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = cr95hf_echo_sync();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "CR95HF echo synchronization failed: %s",
                 esp_err_to_name(err));
        goto done;
    }

    code = 0xFF;
    rsp_len = 0;
    err = cr95hf_transact(CR95HF_CMD_IDN, NULL, 0, &code, rsp, sizeof(rsp),
                          &rsp_len, CR95HF_TIMEOUT_MS);
    if (err == ESP_OK && code == CR95HF_CODE_SUCCESS && rsp_len > 0)
    {
        size_t idn_len = rsp_len < sizeof(idn) - 1 ? rsp_len : sizeof(idn) - 1;
        size_t i;

        for (i = 0; i < idn_len && rsp[i] >= 0x20 && rsp[i] <= 0x7E; i++)
        {
            idn[i] = (char)rsp[i];
        }
        idn[i] = '\0';
        ESP_LOGI(TAG, "95HF IDN: \"%s\" (len=%u ROM=0x%02x)",
                 idn, (unsigned)rsp_len, rsp[rsp_len - 1]);
    }
    else
    {
        ESP_LOGW(TAG, "95HF IDN failed: err=%s code=0x%02x len=%u",
                 esp_err_to_name(err), code, (unsigned)rsp_len);
    }

    /*
     * Start from the documented clean RF state before selecting Type A. This
     * also prevents an inherited field state from a prior incomplete session.
     */
    cr95hf_field_off();
    err = cr95hf_select_iso14443a();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "CR95HF ProtocolSelect failed: %s",
                 esp_err_to_name(err));
        goto done;
    }

    s_ready = true;
    ESP_LOGI(TAG, "CR95HF ready (UART%d %d 8-N-2, ESP TX=%d RX=%d)",
             NFC_UART_PORT, NFC_UART_BAUD, PIN_NFC_TX, PIN_NFC_RX);
    err = ESP_OK;

done:
    if (err != ESP_OK)
    {
        s_ready = false;
        if (driver_installed)
        {
            uart_driver_delete(NFC_UART_PORT);
        }
        /* uart_driver_delete() releases the driver but leaves the GPIO matrix
         * binding intact. Reset both pins so a bounded cr95hf_init() retry
         * repeats the exact first-boot GPIO wake and UART attachment. */
        gpio_reset_pin(PIN_NFC_TX);
        gpio_reset_pin(PIN_NFC_RX);
    }
    xSemaphoreGive(s_uart_mutex);
    return err;
}

/* ------------------------------------------------------------------- poll */

bool cr95hf_poll_card(uint8_t *uid, uint8_t *uid_len, char *url,
                      size_t url_cap, cr95hf_card_info_t *info)
{
    bool ok;
    uint8_t sak = 0;

    if (uid == NULL || uid_len == NULL || *uid_len == 0)
    {
        return false;
    }
    if (s_uart_mutex == NULL || !s_ready)
    {
        return false;
    }
    if (url != NULL && url_cap > 0)
    {
        url[0] = '\0';
    }
    if (info != NULL)
    {
        memset(info, 0, sizeof(*info));
        info->state = CR95HF_CARD_UNREADABLE;
    }

    xSemaphoreTake(s_uart_mutex, portMAX_DELAY);

    ok = cr95hf_activate(uid, uid_len, &sak);
    if (ok)
    {
        if (info != NULL)
        {
            info->sak = sak;
            if (cr95hf_read_static_locks(&info->lock0, &info->lock1) == ESP_OK)
            {
                info->state = cr95hf_read_card(url, url_cap, info);
                if (info->state == CR95HF_CARD_BLANK
                    && cr95hf_page_is_locked(CR95HF_USER_PAGE, info->lock0,
                                              info->lock1))
                {
                    info->state = CR95HF_CARD_LOCKED;
                }
            }
            else
            {
                ESP_LOGW(TAG, "Type 2 static-lock read failed");
            }
        }
        else if (url != NULL && url_cap > 0)
        {
            /* URL read is best-effort: a tag without a URI still yields its UID. */
            cr95hf_read_url(url, url_cap);
        }
    }
    cr95hf_field_off();

    xSemaphoreGive(s_uart_mutex);
    return ok;
}

bool cr95hf_poll(uint8_t *uid, uint8_t *uid_len, char *url, size_t url_cap)
{
    return cr95hf_poll_card(uid, uid_len, url, url_cap, NULL);
}

/* --------------------------------------------------------------- write url */

esp_err_t cr95hf_write_url(const char *url, const uint8_t *expected_uid,
                           uint8_t expected_uid_len)
{
    uint8_t uid[CR95HF_UID_MAX];
    uint8_t uid_len = sizeof(uid);
    char readback[CR95HF_URL_MAX + 1];
    size_t capacity;
    bool cc_writable;
    bool cc_read_ok;
    uint8_t cc[4];
    uint8_t lock0;
    uint8_t lock1;
    esp_err_t err;

    if (url == NULL || url[0] == '\0' || expected_uid == NULL
        || expected_uid_len == 0 || expected_uid_len > sizeof(uid))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_uart_mutex == NULL || !s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    if (!cr95hf_activate(uid, &uid_len, NULL))
    {
        err = ESP_ERR_NOT_FOUND;
        goto done;
    }
    if (uid_len != expected_uid_len
        || memcmp(uid, expected_uid, uid_len) != 0)
    {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }

    err = cr95hf_read_static_locks(&lock0, &lock1);
    if (err != ESP_OK)
    {
        goto done;
    }
    if (!cr95hf_read_cc(&capacity, &cc_writable, &cc_read_ok, cc))
    {
        if (!cc_read_ok)
        {
            err = ESP_FAIL;
            goto done;
        }

        err = cr95hf_format_blank_mf0ul11(cc, lock0, lock1, expected_uid,
                                          expected_uid_len);
        if (err != ESP_OK)
        {
            goto done;
        }
        capacity = CR95HF_MF0UL11_CAPACITY;
    }
    else if (!cc_writable)
    {
        ESP_LOGE(TAG, "Type 2 capability container declares read-only access");
        err = ESP_ERR_NOT_ALLOWED;
        goto done;
    }

    err = cr95hf_write_ndef(url, capacity, lock0, lock1, expected_uid,
                            expected_uid_len);
    if (err != ESP_OK)
    {
        goto done;
    }

    readback[0] = '\0';
    if (!cr95hf_read_url(readback, sizeof(readback)))
    {
        ESP_LOGE(TAG, "NDEF write verification read failed");
        err = ESP_FAIL;
    }
    else if (strcmp(readback, url) != 0)
    {
        ESP_LOGE(TAG, "NDEF write verification mismatch: got \"%s\"",
                 readback);
        err = ESP_FAIL;
    }
    else
    {
        ESP_LOGI(TAG, "NDEF write verified: %s", readback);
    }

done:
    cr95hf_field_off();
    xSemaphoreGive(s_uart_mutex);
    return err;
}
