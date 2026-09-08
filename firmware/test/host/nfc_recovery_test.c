/* Host regressions for CR95HF UART recovery and complete Type-A polling.
 * From the repository root:
 * cc -std=c11 -Wall -Wextra -Werror \
 *   -Ifirmware/test/host/nfc_stubs -Ifirmware/test/host/stubs \
 *   firmware/test/host/nfc_recovery_test.c -o /tmp/nfc_recovery_test
 * /tmp/nfc_recovery_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                           \
    do                                             \
    {                                              \
        if (!(cond))                               \
        {                                          \
            fprintf(stderr, "FAIL: ");             \
            fprintf(stderr, __VA_ARGS__);           \
            fprintf(stderr, "\n");                \
            exit(1);                               \
        }                                          \
    } while (0)

#include "../../components/cr95hf/cr95hf.c"

typedef enum
{
    READER_NORMAL,
    READER_INPUT_BUFFER_FULL,
    READER_BUFFERED_MISMATCH,
    READER_ABSENT,
} reader_mode_t;

typedef enum
{
    RF_FAULT_NONE,
    RF_FAULT_UART_TIMEOUT,
    RF_FAULT_UART_SHORT_WRITE,
    RF_FAULT_UART_TRUNCATED,
    RF_FAULT_SHORT_FRAME,
    RF_FAULT_CRC,
    RF_FAULT_BCC,
    RF_FAULT_ERROR,
    RF_FAULT_MALFORMED_TIMEOUT,
} rf_fault_t;

static struct
{
    reader_mode_t mode;
    unsigned echoes;
    unsigned installs;
    unsigned reads;
    uint64_t elapsed_us;
    uint64_t field_on_us;
    bool installed;
    bool synchronized;
    bool field_on;
    bool tag_present;
    bool halted;
    bool selected;
    bool protocol_error;
    unsigned requests;
    unsigned user_reads;
    unsigned tag_reads;
    size_t accessible_bytes;
    uint8_t memory[512];
    rf_fault_t fault;
    uint8_t fault_command;
    uint8_t fault_page;
    uint8_t rx[64];
    size_t rx_pos;
    size_t rx_len;
} reader;

static void queue_reply(const uint8_t *data, size_t len)
{
    CHECK(reader.rx_pos == reader.rx_len, "reader reply overwritten");
    CHECK(len <= sizeof(reader.rx), "reader reply exceeds simulated FIFO");
    memcpy(reader.rx, data, len);
    reader.rx_pos = 0;
    reader.rx_len = len;
}

/* Supply the CR95HF status trailer, including the hardware CRC-check result. */
static void queue_rf_reply(const uint8_t *data, size_t len, bool with_crc,
                            rf_fault_t fault)
{
    uint8_t frame[32] = { 0x80, 0 };
    size_t status = 2 + len + (with_crc ? 2 : 0);
    size_t total = status + 3;

    memcpy(frame + 2, data, len);
    frame[status] = with_crc ? 0x08 : 0x28;
    if (fault == RF_FAULT_BCC)
    {
        frame[2 + len - 1] ^= 1;
    }
    else if (fault == RF_FAULT_CRC)
    {
        frame[status] |= 0x20;
    }
    else if (fault == RF_FAULT_SHORT_FRAME)
    {
        /* Complete UART reply carrying an incomplete RF frame. */
        total--;
    }
    frame[1] = (uint8_t)(total - 2);
    if (fault == RF_FAULT_UART_TRUNCATED)
    {
        /* Advertised length stays intact; the required last byte never arrives. */
        total--;
    }
    queue_reply(frame, total);
}

static int exchange_rf(const uint8_t *rf, size_t len)
{
    const uint8_t timeout[] = { 0x87, 0 };
    rf_fault_t fault = rf[0] == reader.fault_command
        && (rf[0] != 0x30 || rf[1] == reader.fault_page)
        ? reader.fault : RF_FAULT_NONE;

    CHECK(reader.field_on, "RF command with field off");
    if (reader.elapsed_us - reader.field_on_us < 10000)
    {
        /* A tag cannot answer before its power-up guard has elapsed. */
        queue_reply(timeout, sizeof(timeout));
        return (int)len + 2;
    }
    if (fault == RF_FAULT_UART_SHORT_WRITE)
    {
        return (int)len + 1;
    }
    if (fault == RF_FAULT_UART_TIMEOUT)
    {
        return (int)len + 2;
    }
    if (fault == RF_FAULT_ERROR)
    {
        const uint8_t error[] = { 0x8E, 0 };
        queue_reply(error, sizeof(error));
        return (int)len + 2;
    }
    if (fault == RF_FAULT_MALFORMED_TIMEOUT)
    {
        const uint8_t malformed[] = { 0x87, 1, 0 };
        queue_reply(malformed, sizeof(malformed));
        return (int)len + 2;
    }
    if (rf[0] == 0x26 || rf[0] == 0x52)
    {
        const uint8_t atqa[] = { 0x04, 0 };
        CHECK(len == 2 && rf[1] == 0x07, "invalid short request frame");
        reader.requests++;
        if (!reader.tag_present || (reader.halted && rf[0] == 0x26))
        {
            queue_reply(timeout, sizeof(timeout));
        }
        else
        {
            reader.halted = false;
            queue_rf_reply(atqa, sizeof(atqa), false, fault);
        }
    }
    else if (rf[0] == 0x93 && rf[1] == 0x20)
    {
        const uint8_t uid[] = { 0x01, 0x02, 0x03, 0x04, 0x04 };
        CHECK(reader.tag_present && !reader.halted, "anticollision without tag");
        CHECK(len == 3 && rf[2] == 0x08, "invalid anticollision frame");
        queue_rf_reply(uid, sizeof(uid), false, fault);
    }
    else if (rf[0] == 0x93 && rf[1] == 0x70)
    {
        const uint8_t sak[] = { 0 };
        CHECK(len == 8 && rf[7] == 0x28, "invalid select frame");
        reader.selected = true;
        queue_rf_reply(sak, sizeof(sak), true, fault);
    }
    else if (rf[0] == 0x30)
    {
        size_t start = (size_t)rf[1] * 4;
        reader.tag_reads++;
        CHECK(reader.selected, "memory read before UID selection");
        CHECK(len == 3 && rf[2] == 0x28, "invalid memory read frame");
        if (rf[1] >= 4)
        {
            reader.user_reads++;
        }
        if (start + 16 > reader.accessible_bytes)
        {
            queue_reply(timeout, sizeof(timeout));
        }
        else
        {
            queue_rf_reply(reader.memory + start, 16, true, fault);
        }
    }
    else
    {
        CHECK(false, "unexpected RF command 0x%02x", rf[0]);
    }
    return (int)len + 2;
}

esp_err_t uart_driver_install(uart_port_t port, int rx_size, int tx_size,
                               int queue_size, void *queue, int flags)
{
    (void)port;
    (void)rx_size;
    (void)tx_size;
    (void)queue_size;
    (void)queue;
    (void)flags;
    CHECK(++reader.installs == 1, "recovery restarted UART initialization");
    reader.installed = true;
    return ESP_OK;
}

esp_err_t uart_driver_delete(uart_port_t port)
{
    (void)port;
    reader.installed = false;
    return ESP_OK;
}

esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config)
{
    (void)port;
    (void)config;
    return ESP_OK;
}

esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts)
{
    (void)port;
    (void)tx;
    (void)rx;
    (void)rts;
    (void)cts;
    return ESP_OK;
}

esp_err_t uart_flush_input(uart_port_t port)
{
    (void)port;
    reader.rx_pos = reader.rx_len = 0;
    return ESP_OK;
}

int uart_write_bytes(uart_port_t port, const void *data, size_t len)
{
    const uint8_t *frame = data;
    (void)port;
    CHECK(reader.installed, "transaction without a UART driver");
    CHECK(len != 0, "empty reader command");
    if (len == 2 && frame[0] == 0x00 && frame[1] == 0x55)
    {
        /* The internal stock 00 marker is not part of the UART Echo frame. */
        reader.synchronized = false;
        return (int)len;
    }
    if (frame[0] == 0x55 && len == 1)
    {
        CHECK(++reader.echoes <= 530, "absent reader exceeded recovery bound");
        if (reader.mode == READER_BUFFERED_MISMATCH && reader.echoes == 529)
        {
            /* A stale framed reply, including a payload byte equal to Echo.
             * It must be drained, not mistaken for synchronization. */
            const uint8_t mismatch[] = { 0x87, 0x02, 0xAA, 0x55 };
            queue_reply(mismatch, sizeof(mismatch));
        }
        else if (reader.mode == READER_NORMAL
                 || (reader.mode == READER_INPUT_BUFFER_FULL
                     && reader.echoes >= 528)
                 || (reader.mode == READER_BUFFERED_MISMATCH
                     && reader.echoes >= 530))
        {
            const uint8_t echo[] = { 0x55 };
            reader.synchronized = true;
            queue_reply(echo, sizeof(echo));
        }
        return (int)len;
    }

    CHECK(len >= 2, "incomplete framed reader command");
    CHECK(reader.synchronized, "normal command sent before reader recovered");
    CHECK(len == (size_t)frame[1] + 2, "invalid normal command framing");
    if (frame[0] == 0x01 && frame[1] == 0)
    {
        const uint8_t idn[] = { 0x00, 0x01, 'R' };
        queue_reply(idn, sizeof(idn));
    }
    else if (frame[0] == 0x02 && frame[1] == 2 && frame[3] == 0
             && (frame[2] == 0 || frame[2] == 2))
    {
        const uint8_t selected[] = { 0x00, 0x00 };
        if (reader.protocol_error && frame[2] == 2)
        {
            const uint8_t error[] = { 0x82, 0 };
            queue_reply(error, sizeof(error));
        }
        else
        {
            reader.field_on = frame[2] == 2;
            reader.field_on_us = reader.elapsed_us;
            reader.selected = false;
            queue_reply(selected, sizeof(selected));
        }
    }
    else if (frame[0] == 0x04 && frame[1] >= 2)
    {
        return exchange_rf(frame + 2, frame[1]);
    }
    else
    {
        CHECK(false, "unexpected reader command 0x%02x", frame[0]);
    }
    return (int)len;
}

int uart_read_bytes(uart_port_t port, void *data, uint32_t len,
                    TickType_t timeout)
{
    size_t available = reader.rx_len - reader.rx_pos;
    (void)port;
    CHECK(++reader.reads <= 2000, "reader recovery did not terminate");
    if (available == 0)
    {
        reader.elapsed_us += (uint64_t)timeout * 1000;
        return 0;
    }
    size_t received = len < available ? len : available;
    memcpy(data, reader.rx + reader.rx_pos, received);
    reader.rx_pos += received;
    return (int)received;
}

esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode)
{
    (void)pin;
    (void)mode;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    (void)pin;
    (void)level;
    return ESP_OK;
}

esp_err_t gpio_reset_pin(gpio_num_t pin)
{
    (void)pin;
    return ESP_OK;
}


void vTaskDelay(TickType_t ticks)
{
    reader.elapsed_us += (uint64_t)ticks * 1000;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return &reader;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    (void)sem;
    (void)ticks;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    (void)sem;
    return pdTRUE;
}

const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

static void reset_reader(reader_mode_t mode)
{
    memset(&reader, 0, sizeof(reader));
    reader.mode = mode;
    s_ready = false;
    reader.accessible_bytes = sizeof(reader.memory);
}

static void prepare_card(const uint8_t *tlv, size_t tlv_len,
                          uint8_t capacity_units, size_t accessible_bytes)
{
    reset_reader(READER_NORMAL);
    CHECK(cr95hf_init() == ESP_OK, "poll fixture initialization failed");
    reader.tag_present = true;
    reader.accessible_bytes = accessible_bytes;
    reader.memory[12] = 0xE1;
    reader.memory[13] = 0x10;
    reader.memory[14] = capacity_units;
    CHECK(tlv_len <= sizeof(reader.memory) - 16, "fixture TLV too large");
    if (tlv_len != 0)
    {
        memcpy(reader.memory + 16, tlv, tlv_len);
    }
}

static const uint8_t song_tlv[] = {
    0x03, 21, 0xD1, 0x01, 17, 0x55, 0x04,
    'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm', '/', 's', 'o', 'n', 'g',
    0xFE,
};

static void prepare_song(void)
{
    /* CC advertises 496 bytes; pages after the complete NDEF are inaccessible. */
    prepare_card(song_tlv, sizeof(song_tlv), 0x3E, 48);
}

static cr95hf_poll_result_t poll_card(char *url, size_t url_cap,
                                      cr95hf_card_info_t *info)
{
    uint8_t uid[CR95HF_UID_MAX];
    uint8_t uid_len = sizeof(uid);
    const uint8_t expected_uid[] = { 1, 2, 3, 4 };
    cr95hf_poll_result_t result;

    memset(uid, 0xA5, sizeof(uid));
    snprintf(url, url_cap, "stale URI");
    result = cr95hf_poll_card(uid, &uid_len, url, url_cap, info);
    CHECK(!reader.field_on, "poll left RF field on");
    if (result == CR95HF_POLL_PRESENT)
    {
        CHECK(uid_len == sizeof(expected_uid)
              && memcmp(uid, expected_uid, sizeof(expected_uid)) == 0,
              "poll published an invalid UID");
    }
    else
    {
        CHECK(url[0] == '\0', "failed activation retained a stale URL");
    }
    return result;
}

static void test_bounded_payload(void)
{
    char url[CR95HF_URL_MAX + 1];
    cr95hf_card_info_t info;
    const uint8_t terminator[] = { 0xFE };
    const uint8_t incomplete[] = { 0x03, 20, 0xD1, 1, 16, 0x55, 0x04 };
    const uint8_t malformed_record[] = { 0x03, 5, 0xD1, 1, 20, 0x55, 0x04 };

    prepare_song();
    CHECK(poll_card(url, sizeof(url), &info) == CR95HF_POLL_PRESENT
          && info.state == CR95HF_CARD_URI
          && strcmp(url, "https://example.com/song") == 0,
          "diagnostics forced a read beyond the complete NDEF");
    CHECK(reader.user_reads == 2 && info.raw_ndef_len == 32
          && info.capacity == 496
          && memcmp(info.raw_ndef, song_tlv, sizeof(song_tlv)) == 0,
          "bounded acquisition lost the complete NDEF prefix or card capacity");

    prepare_song();
    CHECK(poll_card(url, sizeof(url), NULL) == CR95HF_POLL_PRESENT
          && strcmp(url, "https://example.com/song") == 0
          && reader.user_reads == 2,
          "plain URL acquisition did not stop at the complete TLV");

    prepare_song();
    reader.accessible_bytes = 0;
    uint8_t uid[CR95HF_UID_MAX];
    uint8_t uid_len = sizeof(uid);
    CHECK(cr95hf_poll_card(uid, &uid_len, NULL, 0, NULL) == CR95HF_POLL_PRESENT
          && uid_len == 4 && memcmp(uid, (uint8_t[]){1, 2, 3, 4}, 4) == 0
          && reader.tag_reads == 0 && !reader.field_on,
          "UID-only presence must not read inaccessible tag memory");

    prepare_card(terminator, sizeof(terminator), 0x3E, 32);
    CHECK(poll_card(url, sizeof(url), &info) == CR95HF_POLL_PRESENT
          && info.state == CR95HF_CARD_BLANK && url[0] == '\0'
          && reader.user_reads == 1,
          "proven empty NDEF unnecessarily read inaccessible trailing pages");

    prepare_card(NULL, 0, 2, 32);
    CHECK(poll_card(url, sizeof(url), &info) == CR95HF_POLL_PRESENT
          && info.state == CR95HF_CARD_BLANK,
          "a completely read all-NULL user area was not recognized as blank");

    prepare_card(NULL, 0, 0x3E, 512);
    CHECK(poll_card(url, sizeof(url), &info) == CR95HF_POLL_PRESENT
          && info.state == CR95HF_CARD_UNREADABLE && url[0] == '\0'
          && info.raw_ndef_len == 256 && info.capacity == 496,
          "scan limit was mistaken for the end of an empty card");

    prepare_card(incomplete, sizeof(incomplete), 2, 32);
    CHECK(poll_card(url, sizeof(url), &info) == CR95HF_POLL_PRESENT
          && info.state != CR95HF_CARD_BLANK && url[0] == '\0',
          "truncated TLV became blank or exposed a URI");

    prepare_card(malformed_record, sizeof(malformed_record), 2, 32);
    CHECK(poll_card(url, sizeof(url), &info) == CR95HF_POLL_PRESENT
          && info.state != CR95HF_CARD_BLANK && url[0] == '\0',
          "malformed NDEF record became blank or exposed a URI");

    const rf_fault_t required_read_faults[] = {
        RF_FAULT_UART_TRUNCATED, RF_FAULT_SHORT_FRAME,
        RF_FAULT_UART_TIMEOUT, RF_FAULT_CRC,
    };
    for (size_t i = 0; i < sizeof(required_read_faults) / sizeof(required_read_faults[0]); i++)
    {
        prepare_song();
        reader.fault_command = 0x30;
        reader.fault_page = 8;
        reader.fault = required_read_faults[i];
        CHECK(poll_card(url, sizeof(url), &info) == CR95HF_POLL_PRESENT
              && info.state == CR95HF_CARD_UNREADABLE && url[0] == '\0'
              && info.raw_ndef_len == 16,
              "failed required read (%u) became a valid URI/blank card",
              (unsigned)required_read_faults[i]);
    }
}

static void test_poll_classification(void)
{
    char url[CR95HF_URL_MAX + 1];
    cr95hf_card_info_t info;
    const uint8_t expected_uid[] = { 1, 2, 3, 4 };
    const rf_fault_t request_faults[] = {
        RF_FAULT_UART_TIMEOUT, RF_FAULT_UART_SHORT_WRITE,
        RF_FAULT_UART_TRUNCATED, RF_FAULT_SHORT_FRAME,
        RF_FAULT_ERROR, RF_FAULT_MALFORMED_TIMEOUT,
    };

    prepare_song();
    reader.tag_present = false;
    CHECK(poll_card(url, sizeof(url), &info) == CR95HF_POLL_ABSENT
          && reader.requests == 2,
          "two actual no-response results did not establish absence");
    CHECK(cr95hf_write_url("https://example.com/song", expected_uid,
                           sizeof(expected_uid)) == ESP_ERR_NOT_FOUND,
          "writer did not distinguish an absent card");

    prepare_song();
    reader.halted = true;
    CHECK(poll_card(url, sizeof(url), &info) == CR95HF_POLL_PRESENT
          && reader.requests == 2 && info.state == CR95HF_CARD_URI,
          "WUPA failed to activate a halted tag after REQA no-response");

    for (size_t i = 0; i < sizeof(request_faults) / sizeof(request_faults[0]); i++)
    {
        prepare_song();
        reader.fault_command = 0x26;
        reader.fault = request_faults[i];
        CHECK(poll_card(url, sizeof(url), &info) == CR95HF_POLL_ERROR,
              "REQA fault (%u) became absence or presence",
              (unsigned)request_faults[i]);
    }

    prepare_song();
    reader.tag_present = false;
    reader.fault_command = 0x52;
    reader.fault = RF_FAULT_UART_TIMEOUT;
    CHECK(poll_card(url, sizeof(url), &info) == CR95HF_POLL_ERROR,
          "REQA no-response plus WUPA transport timeout became absence");
    CHECK(cr95hf_write_url("https://example.com/song", expected_uid,
                           sizeof(expected_uid)) == ESP_FAIL,
          "writer treated activation transport failure as absence/presence");

    prepare_song();
    reader.fault_command = 0x93;
    reader.fault = RF_FAULT_BCC;
    CHECK(poll_card(url, sizeof(url), &info) == CR95HF_POLL_ERROR,
          "invalid UID BCC became presence");

    prepare_song();
    reader.fault_command = 0x93;
    reader.fault = RF_FAULT_CRC;
    CHECK(poll_card(url, sizeof(url), &info) == CR95HF_POLL_ERROR,
          "CRC-invalid selection became presence");

    prepare_song();
    reader.protocol_error = true;
    CHECK(poll_card(url, sizeof(url), &info) == CR95HF_POLL_ERROR,
          "ProtocolSelect failure became absence");
}

int main(void)
{
    reset_reader(READER_NORMAL);
    CHECK(cr95hf_init() == ESP_OK && reader.field_on,
          "normally framed reader failed initialization");

    /* The datasheet's maximum input-buffer recovery exceeds the old 255 cap. */
    reset_reader(READER_INPUT_BUFFER_FULL);
    CHECK(cr95hf_init() == ESP_OK && reader.field_on,
          "reader needing 528 Echo commands failed its first initialization");

    /* Exercise the final permitted attempt, after a buffered mismatched reply. */
    reset_reader(READER_BUFFERED_MISMATCH);
    CHECK(cr95hf_init() == ESP_OK && reader.field_on,
          "buffered mismatched reply prevented recovery on attempt 530");

    reset_reader(READER_ABSENT);
    CHECK(cr95hf_init() == ESP_ERR_TIMEOUT,
          "absent reader did not report synchronization timeout");
    CHECK(reader.elapsed_us <= 7000000,
          "absent reader exceeded the seven-second initialization bound");
    CHECK(!reader.installed && !reader.field_on,
          "absent-reader timeout left reader resources active");

    printf("NFC recovery: normal, 528-Echo recovery, buffered reply/530 boundary, "
           "and absent-reader timeout passed (%u attempts, %llu us absent)\n",
           reader.echoes, (unsigned long long)reader.elapsed_us);
    test_bounded_payload();
    test_poll_classification();
    printf("NFC polling: bounded NDEF, required-frame rejection, power-up guard, "
           "and absence/error classification passed\n");
    return 0;
}
