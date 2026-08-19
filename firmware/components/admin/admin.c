/*
 * admin.c — Admin mode: SoftAP + HTTP web UI for offline content management.
 *
 * Lifecycle: admin_start() mounts the content store, brings up the Wi-Fi
 * stack and an open AP, pins a static address, starts esp_http_server, and
 * issues a random 4-digit access code. admin_stop() reverses that. The code is
 * exchanged at POST /api/login for a session cookie (yoto_session), which
 * gates POST /api/add and POST /api/delete; GET / and GET /api/list stay open
 * so the page and content list always render.
 *
 * Depends on the content component (content.h) for the SD card, media files
 * (/sdcard/media/) and mapping.json (/sdcard/mapping.json). Media received by
 * /api/add is written to /sdcard/media/<name> here and registered through
 * content_add(); mapping persistence is content's responsibility.
 */
#include "admin.h"
#include "content.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

static const char *TAG = "admin";

/* SoftAP parameters (open network, fixed channel). */
#define ADMIN_SSID              "openyoto"
#define ADMIN_CHANNEL           1
#define ADMIN_MAX_CONN          4
#define ADMIN_COOKIE_NAME       "yoto_session"

/* Content store layout owned jointly with the content component. */
#define ADMIN_MEDIA_DIR         "/sdcard/media"
#define ADMIN_PATH_MAX          256
#define ADMIN_NAME_MAX          96
#define ADMIN_URL_MAX           512
#define ADMIN_LIST_MAX          16384
#define ADMIN_BODY_MAX          (4 * 1024 * 1024)
#define ADMIN_BOUNDARY_MAX      128

/* Active-session state. */
static bool             s_active;
static bool             s_netif_ready;
static bool             s_wifi_inited;
static bool             s_wifi_started;
static uint16_t         s_code;
static char             s_code_str[8];
static char             s_session_token[33];
static httpd_handle_t   s_server;
static esp_netif_t     *s_ap_netif;
static admin_code_cb_t  s_code_cb;

/* ------------------------------------------------------------------ page -- */

/* The single-page admin UI is embedded via ESP-IDF EMBED_FILES (see
 * CMakeLists.txt); the linker exposes it as these byte symbols. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* A parsed add request: multipart fills raw byte references, JSON fills URL or
 * data-URL spec strings. Exactly one representation is used per media field. */
typedef struct
{
    char             url[ADMIN_URL_MAX];
    const char      *sound_spec;
    const char      *image_spec;
    const uint8_t   *sound_data;
    size_t           sound_len;
    char             sound_ext[16];
    const uint8_t   *image_data;
    size_t           image_len;
    char             image_ext[16];
} admin_add_req_t;

/* --------------------------------------------------------------- helpers -- */

/** Length-bounded memmem: locate `needle` within `hay` (may contain NULs). */
static const char *admin_find(const char *hay, size_t hay_len,
                              const char *needle, size_t needle_len)
{
    size_t i;

    if (needle_len == 0 || hay_len < needle_len)
    {
        return NULL;
    }
    for (i = 0; i + needle_len <= hay_len; i++)
    {
        if (memcmp(hay + i, needle, needle_len) == 0)
        {
            return hay + i;
        }
    }
    return NULL;
}

/** Copy `src_len` bytes of a form field into `dst`, trimming trailing CR/LF/space. */
static void admin_copy_field(char *dst, size_t dst_len,
                             const char *src, size_t src_len)
{
    size_t n = src_len;

    if (dst_len == 0)
    {
        return;
    }
    while (n > 0 && (src[n - 1] == '\r' || src[n - 1] == '\n' || src[n - 1] == ' '))
    {
        n--;
    }
    if (n >= dst_len)
    {
        n = dst_len - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/** Extract a quoted `name="..."` / `filename="..."` parameter from a header block. */
static void admin_header_param(const char *headers, const char *key,
                               size_t key_len, char *out, size_t out_len)
{
    const char *k = strstr(headers, key);
    size_t i = 0;

    if (k == NULL)
    {
        return;
    }
    k += key_len;
    while (*k != '\0' && *k != '"' && i + 1 < out_len)
    {
        out[i++] = *k++;
    }
    out[i] = '\0';
}

/** Extract a plain `Key: value` header (stops at CR/LF and `;`). */
static void admin_header_value(const char *headers, const char *key,
                               char *out, size_t out_len)
{
    const char *k = strstr(headers, key);
    size_t i = 0;

    if (k == NULL)
    {
        return;
    }
    k += strlen(key);
    while (*k == ' ')
    {
        k++;
    }
    while (*k != '\0' && *k != '\r' && *k != '\n' && *k != ';' && i + 1 < out_len)
    {
        out[i++] = *k++;
    }
    out[i] = '\0';
}

/** Derive a lowercase media extension from an uploaded filename. */
static bool admin_ext_from_filename(const char *filename, char *ext, size_t ext_len)
{
    const char *dot;
    size_t n;

    if (filename == NULL)
    {
        return false;
    }
    dot = strrchr(filename, '.');
    if (dot == NULL)
    {
        return false;
    }
    n = strlen(dot);
    if (n < 2 || n >= ext_len)
    {
        return false;
    }
    memcpy(ext, dot, n + 1);
    return true;
}

/** Map a MIME type to a media extension. */
static bool admin_ext_from_mime(const char *mime, char *ext, size_t ext_len)
{
    if (mime == NULL)
    {
        return false;
    }
    if (strstr(mime, "image/png") != NULL)
    {
        snprintf(ext, ext_len, ".png");
        return true;
    }
    if (strstr(mime, "jpeg") != NULL || strstr(mime, "jpg") != NULL)
    {
        snprintf(ext, ext_len, ".jpg");
        return true;
    }
    if (strstr(mime, "image/gif") != NULL)
    {
        snprintf(ext, ext_len, ".gif");
        return true;
    }
    if (strstr(mime, "image/webp") != NULL)
    {
        snprintf(ext, ext_len, ".webp");
        return true;
    }
    if (strstr(mime, "mpeg") != NULL || strstr(mime, "mp3") != NULL)
    {
        snprintf(ext, ext_len, ".mp3");
        return true;
    }
    if (strstr(mime, "wav") != NULL)
    {
        snprintf(ext, ext_len, ".wav");
        return true;
    }
    if (strstr(mime, "aac") != NULL)
    {
        snprintf(ext, ext_len, ".aac");
        return true;
    }
    if (strstr(mime, "ogg") != NULL || strstr(mime, "opus") != NULL)
    {
        snprintf(ext, ext_len, ".ogg");
        return true;
    }
    return false;
}

/** Derive an extension from the last path segment of a URL (before the query). */
static bool admin_ext_from_url(const char *url, char *ext, size_t ext_len)
{
    const char *end;
    const char *q;
    const char *dot = NULL;
    const char *slash;
    const char *p;
    size_t n;

    if (url == NULL)
    {
        return false;
    }
    end = url + strlen(url);
    q = strchr(url, '?');
    if (q != NULL)
    {
        end = q;
    }
    slash = url;
    for (p = url; p < end; p++)
    {
        if (*p == '/')
        {
            slash = p;
            dot = NULL;
        }
        else if (*p == '.')
        {
            dot = p;
        }
    }
    if (dot == NULL || dot <= slash)
    {
        return false;
    }
    n = (size_t)(end - dot);
    if (n < 2 || n >= ext_len)
    {
        return false;
    }
    memcpy(ext, dot, n);
    ext[n] = '\0';
    return true;
}

/** Pick a media extension from a filename, else a MIME type, else a fallback. */
static void admin_media_ext(char *ext, size_t ext_len, const char *filename,
                            const char *mime, const char *fallback)
{
    if (admin_ext_from_filename(filename, ext, ext_len))
    {
        return;
    }
    if (admin_ext_from_mime(mime, ext, ext_len))
    {
        return;
    }
    snprintf(ext, ext_len, "%s", fallback);
}

/** Write raw media bytes to /sdcard/media/<random-name><ext>. */
static esp_err_t admin_save_media(const uint8_t *data, size_t len,
                                  const char *ext, char *name_out, size_t name_out_len)
{
    char fname[ADMIN_NAME_MAX];
    char path[ADMIN_PATH_MAX];
    FILE *fp;
    size_t written;

    snprintf(fname, sizeof(fname), "%08lx%s", (unsigned long)esp_random(), ext);
    snprintf(path, sizeof(path), ADMIN_MEDIA_DIR "/%s", fname);

    fp = fopen(path, "wb");
    if (fp == NULL)
    {
        ESP_LOGE(TAG, "cannot open %s for writing", path);
        return ESP_ERR_NOT_FOUND;
    }
    written = fwrite(data, 1, len, fp);
    fclose(fp);
    if (written != len)
    {
        unlink(path);
        return ESP_FAIL;
    }
    snprintf(name_out, name_out_len, "%s", fname);
    ESP_LOGI(TAG, "saved media %s (%u bytes)", path, (unsigned)len);
    return ESP_OK;
}

/** Download `url` to a local file via esp_http_client. */
static esp_err_t admin_download_to_file(const char *url, const char *path)
{
    esp_http_client_config_t cfg;
    esp_http_client_handle_t client;
    esp_err_t err;
    int content_len;
    FILE *fp;
    char buf[1024];
    int read;

    memset(&cfg, 0, sizeof(cfg));
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 20000;

    client = esp_http_client_init(&cfg);
    if (client == NULL)
    {
        return ESP_FAIL;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        esp_http_client_cleanup(client);
        return err;
    }

    content_len = esp_http_client_fetch_headers(client);
    if (content_len < 0)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    fp = fopen(path, "wb");
    if (fp == NULL)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_FOUND;
    }

    while ((read = esp_http_client_read(client, buf, sizeof(buf))) > 0)
    {
        fwrite(buf, 1, (size_t)read, fp);
    }
    fclose(fp);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

/** Save a remote URL, naming the media by URL extension or a fallback. */
static esp_err_t admin_save_remote(const char *url, const char *fallback_ext,
                                   char *name_out, size_t name_out_len)
{
    char ext[16];
    char fname[ADMIN_NAME_MAX];
    char path[ADMIN_PATH_MAX];
    esp_err_t err;

    if (!admin_ext_from_url(url, ext, sizeof(ext)))
    {
        snprintf(ext, sizeof(ext), "%s", fallback_ext);
    }
    snprintf(fname, sizeof(fname), "%08lx%s", (unsigned long)esp_random(), ext);
    snprintf(path, sizeof(path), ADMIN_MEDIA_DIR "/%s", fname);

    err = admin_download_to_file(url, path);
    if (err != ESP_OK)
    {
        return err;
    }
    snprintf(name_out, name_out_len, "%s", fname);
    return ESP_OK;
}

/** Save a `data:<mime>;base64,...` string, decoding the payload to disk. */
static esp_err_t admin_save_data_url(const char *spec, const char *fallback_ext,
                                     char *name_out, size_t name_out_len)
{
    const char *comma = strstr(spec, "base64,");
    const char *mime_start;
    const char *mime_end;
    char mime[64] = { 0 };
    char ext[16];
    size_t b64_len;
    size_t out_cap;
    uint8_t *decoded;
    size_t out_len = 0;
    int rc;
    esp_err_t err;

    if (comma == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    comma += 7; /* skip "base64," */

    mime_start = spec + 5; /* skip "data:" */
    mime_end = strstr(spec, ";");
    if (mime_end != NULL && (size_t)(mime_end - mime_start) < sizeof(mime))
    {
        memcpy(mime, mime_start, (size_t)(mime_end - mime_start));
    }
    if (!admin_ext_from_mime(mime[0] ? mime : NULL, ext, sizeof(ext)))
    {
        snprintf(ext, sizeof(ext), "%s", fallback_ext);
    }

    b64_len = strlen(comma);
    out_cap = (b64_len * 3) / 4 + 4;
    decoded = malloc(out_cap);
    if (decoded == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    rc = mbedtls_base64_decode(decoded, out_cap, &out_len,
                               (const unsigned char *)comma, b64_len);
    if (rc != 0)
    {
        free(decoded);
        return ESP_FAIL;
    }
    err = admin_save_media(decoded, out_len, ext, name_out, name_out_len);
    free(decoded);
    return err;
}

/** Ingest one media spec (data URL, remote URL, or local basename). */
static esp_err_t admin_ingest_spec(const char *spec, const char *fallback_ext,
                                   char *name_out, size_t name_out_len)
{
    const char *base;

    if (strncmp(spec, "data:", 5) == 0)
    {
        return admin_save_data_url(spec, fallback_ext, name_out, name_out_len);
    }
    if (strncmp(spec, "http://", 7) == 0 || strncmp(spec, "https://", 8) == 0)
    {
        return admin_save_remote(spec, fallback_ext, name_out, name_out_len);
    }
    base = strrchr(spec, '/');
    base = (base == NULL) ? spec : base + 1;
    if (*base == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(name_out, name_out_len, "%s", base);
    return ESP_OK;
}

/* ------------------------------------------------------------ multipart -- */

/** Read the whole request body into a heap buffer (bounded, NUL-terminated). */
static esp_err_t admin_read_body(httpd_req_t *req, char **out, size_t *out_len)
{
    size_t total = req->content_len;
    char *buf;
    size_t received = 0;
    int r;

    if (total == 0 || total > ADMIN_BODY_MAX)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    buf = malloc(total + 1);
    if (buf == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    while (received < total)
    {
        r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0)
        {
            free(buf);
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    buf[received] = '\0';
    *out = buf;
    *out_len = received;
    return ESP_OK;
}

/** Extract the multipart boundary token from a Content-Type header value. */
static esp_err_t admin_get_boundary(const char *ct, char *boundary, size_t boundary_len)
{
    const char *b = strstr(ct, "boundary=");
    size_t n;

    if (b == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    b += 9;
    if (*b == '"')
    {
        b++;
    }
    n = strlen(b);
    if (n > 0 && b[n - 1] == '"')
    {
        n--;
    }
    if (n == 0 || n >= boundary_len)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(boundary, b, n);
    boundary[n] = '\0';
    return ESP_OK;
}

/** Parse a multipart/form-data body into an add-request (no media written yet). */
static esp_err_t admin_parse_multipart(char *body, size_t len,
                                       const char *boundary, admin_add_req_t *out)
{
    size_t blen = strlen(boundary);
    char marker[ADMIN_BOUNDARY_MAX + 8];
    size_t marker_len;
    char *p = body;
    size_t remaining = len;

    snprintf(marker, sizeof(marker), "\r\n--%s", boundary);
    marker_len = strlen(marker);

    if (remaining < 2 + blen || strncmp(p, "--", 2) != 0 ||
        strncmp(p + 2, boundary, blen) != 0)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    p += 2 + blen;
    remaining -= 2 + blen;
    if (remaining >= 2 && p[0] == '\r' && p[1] == '\n')
    {
        p += 2;
        remaining -= 2;
    }

    while (remaining > 0)
    {
        const char *hdr_end = admin_find(p, remaining, "\r\n\r\n", 4);
        const char *data;
        size_t data_remaining;
        const char *mark;
        size_t part_len;
        char *hbuf;
        size_t hdr_len;
        char name[64] = { 0 };
        char filename[ADMIN_PATH_MAX] = { 0 };
        char mime[64] = { 0 };

        if (hdr_end == NULL)
        {
            break;
        }
        hdr_len = (size_t)(hdr_end - p);
        data = hdr_end + 4;
        data_remaining = remaining - (size_t)(data - p);

        mark = admin_find(data, data_remaining, marker, marker_len);
        part_len = (mark == NULL) ? data_remaining : (size_t)(mark - data);

        hbuf = malloc(hdr_len + 1);
        if (hbuf == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
        memcpy(hbuf, p, hdr_len);
        hbuf[hdr_len] = '\0';
        admin_header_param(hbuf, "name=\"", 6, name, sizeof(name));
        admin_header_param(hbuf, "filename=\"", 10, filename, sizeof(filename));
        admin_header_value(hbuf, "Content-Type:", mime, sizeof(mime));
        free(hbuf);

        if (strcmp(name, "url") == 0)
        {
            admin_copy_field(out->url, sizeof(out->url), data, part_len);
        }
        else if (strcmp(name, "sound") == 0)
        {
            out->sound_data = (const uint8_t *)data;
            out->sound_len = part_len;
            admin_media_ext(out->sound_ext, sizeof(out->sound_ext), filename, mime, ".mp3");
        }
        else if (strcmp(name, "image") == 0)
        {
            out->image_data = (const uint8_t *)data;
            out->image_len = part_len;
            admin_media_ext(out->image_ext, sizeof(out->image_ext), filename, mime, ".png");
        }

        if (mark == NULL)
        {
            break;
        }
        {
            const char *after_mark = mark + marker_len;
            size_t after_remaining = remaining - (size_t)(after_mark - p);

            if (after_remaining >= 2 && after_mark[0] == '-' && after_mark[1] == '-')
            {
                break;
            }
            if (after_remaining >= 2 && after_mark[0] == '\r' && after_mark[1] == '\n')
            {
                after_mark += 2;
                after_remaining -= 2;
            }
            p = (char *)after_mark;
            remaining = after_remaining;
        }
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------- JSON --- */

/** Parse a raw JSON add request (`url`, `sound`, `image`). */
static esp_err_t admin_parse_json_add(const char *body, admin_add_req_t *out)
{
    cJSON *root = cJSON_Parse(body);
    cJSON *item;

    if (root == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    item = cJSON_GetObjectItem(root, "url");
    if (item != NULL && cJSON_IsString(item))
    {
        snprintf(out->url, sizeof(out->url), "%s", item->valuestring);
    }
    item = cJSON_GetObjectItem(root, "sound");
    if (item != NULL && cJSON_IsString(item))
    {
        out->sound_spec = item->valuestring;
    }
    item = cJSON_GetObjectItem(root, "image");
    if (item != NULL && cJSON_IsString(item))
    {
        out->image_spec = item->valuestring;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/** Route a request body to the correct parser based on Content-Type. */
static esp_err_t admin_parse_add_request(httpd_req_t *req, const char *ct,
                                         admin_add_req_t *out)
{
    esp_err_t err;
    char *body = NULL;
    size_t body_len = 0;

    if (strncmp(ct, "multipart/form-data", 19) == 0)
    {
        char boundary[ADMIN_BOUNDARY_MAX] = { 0 };

        err = admin_get_boundary(ct, boundary, sizeof(boundary));
        if (err != ESP_OK)
        {
            return err;
        }
        err = admin_read_body(req, &body, &body_len);
        if (err != ESP_OK)
        {
            return err;
        }
        err = admin_parse_multipart(body, body_len, boundary, out);
        free(body);
        return err;
    }

    if (strncmp(ct, "application/json", 16) == 0)
    {
        err = admin_read_body(req, &body, &body_len);
        if (err != ESP_OK)
        {
            return err;
        }
        err = admin_parse_json_add(body, out);
        free(body);
        return err;
    }

    return ESP_ERR_INVALID_ARG;
}

/** Persist parsed media (bytes or specs) and register the mapping entry. */
static esp_err_t admin_ingest_add(const admin_add_req_t *add,
                                  char *sound_name, size_t sound_name_len,
                                  char *image_name, size_t image_name_len)
{
    esp_err_t err = ESP_OK;

    if (add->sound_data != NULL && add->sound_len > 0)
    {
        err = admin_save_media(add->sound_data, add->sound_len, add->sound_ext,
                               sound_name, sound_name_len);
    }
    else if (add->sound_spec != NULL && add->sound_spec[0] != '\0')
    {
        err = admin_ingest_spec(add->sound_spec, ".mp3", sound_name, sound_name_len);
    }
    if (err != ESP_OK)
    {
        return err;
    }

    err = ESP_OK;
    if (add->image_data != NULL && add->image_len > 0)
    {
        err = admin_save_media(add->image_data, add->image_len, add->image_ext,
                               image_name, image_name_len);
    }
    else if (add->image_spec != NULL && add->image_spec[0] != '\0')
    {
        err = admin_ingest_spec(add->image_spec, ".png", image_name, image_name_len);
    }
    return err;
}

/* ----------------------------------------------------------------- auth -- */

/** Return true when `pin` matches the active code (string compare). */
static bool admin_pin_match(const char *pin)
{
    return (s_code != 0 && pin != NULL && strcmp(pin, s_code_str) == 0);
}

/** Extract the raw PIN from a login body: JSON {"pin":"..."} or form pin=.... */
static bool admin_extract_pin(const char *body, const char *ct,
                              char *pin, size_t pin_len)
{
    const char *p;
    size_t n;

    if (body == NULL || pin_len == 0)
    {
        return false;
    }
    pin[0] = '\0';

    if (ct != NULL && strncmp(ct, "application/json", 16) == 0)
    {
        cJSON *root = cJSON_Parse(body);
        cJSON *item;

        if (root == NULL)
        {
            return false;
        }
        item = cJSON_GetObjectItem(root, "pin");
        if (item != NULL && cJSON_IsString(item))
        {
            snprintf(pin, pin_len, "%s", item->valuestring);
        }
        cJSON_Delete(root);
        return pin[0] != '\0';
    }

    p = strstr(body, "pin=");
    if (p == NULL)
    {
        return false;
    }
    p += 4;
    n = strcspn(p, "&");
    if (n >= pin_len)
    {
        n = pin_len - 1;
    }
    memcpy(pin, p, n);
    pin[n] = '\0';
    while (n > 0 && (pin[n - 1] == ' ' || pin[n - 1] == '\r' || pin[n - 1] == '\n'))
    {
        pin[--n] = '\0';
    }
    return pin[0] != '\0';
}

/** Generate a fresh 32-hex-char session token into `out` (at least 33 bytes). */
static void admin_new_token(char *out)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t raw[16];
    size_t i;

    esp_fill_random(raw, sizeof(raw));
    for (i = 0; i < sizeof(raw); i++)
    {
        out[2 * i]     = hex[raw[i] >> 4];
        out[2 * i + 1] = hex[raw[i] & 0x0f];
    }
    out[32] = '\0';
}

/** Return true when the request carries a matching yoto_session cookie. */
static bool admin_session_ok(httpd_req_t *req)
{
    char cookie[128] = { 0 };
    size_t cookie_len = httpd_req_get_hdr_value_len(req, "Cookie");
    const char *name = ADMIN_COOKIE_NAME;
    size_t name_len = strlen(name);
    char *cur;
    char *save = NULL;

    if (s_session_token[0] == '\0' || cookie_len == 0 || cookie_len >= sizeof(cookie))
    {
        return false;
    }
    httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie));

    for (cur = strtok_r(cookie, ";", &save); cur != NULL;
         cur = strtok_r(NULL, ";", &save))
    {
        while (*cur == ' ')
        {
            cur++;
        }
        if (strncmp(cur, name, name_len) == 0 && cur[name_len] == '=')
        {
            const char *val = cur + name_len + 1;
            size_t vlen = strlen(val);

            while (vlen > 0 && val[vlen - 1] == ' ')
            {
                vlen--;
            }
            return (vlen == strlen(s_session_token) &&
                    memcmp(val, s_session_token, vlen) == 0);
        }
    }
    return false;
}

/* ------------------------------------------------------------ handlers -- */

static esp_err_t admin_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start,
                           (ssize_t)(index_html_end - index_html_start));
}

static esp_err_t admin_login_handler(httpd_req_t *req)
{
    char ct[64] = { 0 };
    size_t ct_len = httpd_req_get_hdr_value_len(req, "Content-Type");
    char *body = NULL;
    size_t body_len = 0;
    char pin[8] = { 0 };
    char cookie[80];
    esp_err_t err;

    if (ct_len > 0 && ct_len < sizeof(ct))
    {
        httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));
    }

    err = admin_read_body(req, &body, &body_len);
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
        return ESP_FAIL;
    }

    if (!admin_extract_pin(body, ct, pin, sizeof(pin)) || !admin_pin_match(pin))
    {
        free(body);
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
        return ESP_FAIL;
    }
    free(body);

    admin_new_token(s_session_token);
    snprintf(cookie, sizeof(cookie),
             ADMIN_COOKIE_NAME "=%s; Path=/; HttpOnly", s_session_token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t admin_list_handler(httpd_req_t *req)
{
    char *json = malloc(ADMIN_LIST_MAX);
    esp_err_t err;

    if (json == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    err = content_list(json, ADMIN_LIST_MAX);
    if (err != ESP_OK)
    {
        free(json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content list failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t admin_add_handler(httpd_req_t *req)
{
    admin_add_req_t add;
    char ct[256] = { 0 };
    size_t ct_len;
    esp_err_t err;
    char sound_name[ADMIN_NAME_MAX] = { 0 };
    char image_name[ADMIN_NAME_MAX] = { 0 };

    if (!admin_session_ok(req))
    {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
        return ESP_FAIL;
    }

    memset(&add, 0, sizeof(add));

    ct_len = httpd_req_get_hdr_value_len(req, "Content-Type");
    if (ct_len > 0 && ct_len < sizeof(ct))
    {
        httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));
    }

    err = admin_parse_add_request(req, ct, &add);
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
        return ESP_FAIL;
    }
    if (add.url[0] == '\0')
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing url");
        return ESP_FAIL;
    }

    err = admin_ingest_add(&add, sound_name, sizeof(sound_name),
                           image_name, sizeof(image_name));
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "media save failed");
        return ESP_FAIL;
    }

    err = content_add(add.url, sound_name, image_name);
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content add failed");
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t admin_delete_handler(httpd_req_t *req)
{
    char url[ADMIN_URL_MAX] = { 0 };
    char query[512] = { 0 };
    esp_err_t err;
    size_t content_len;

    if (!admin_session_ok(req))
    {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
        return ESP_FAIL;
    }

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        httpd_query_key_value(query, "url", url, sizeof(url));
    }

    content_len = req->content_len;
    if (content_len > 0 && content_len < ADMIN_BODY_MAX)
    {
        char *body = malloc(content_len + 1);

        if (body == NULL)
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
            return ESP_FAIL;
        }
        {
            size_t got = 0;
            int r;

            while (got < content_len)
            {
                r = httpd_req_recv(req, body + got, content_len - got);
                if (r <= 0)
                {
                    break;
                }
                got += (size_t)r;
            }
            body[got] = '\0';

            {
                cJSON *root = cJSON_Parse(body);

                if (root != NULL)
                {
                    cJSON *ju = cJSON_GetObjectItem(root, "url");
                    if (ju != NULL && cJSON_IsString(ju))
                    {
                        snprintf(url, sizeof(url), "%s", ju->valuestring);
                    }
                    cJSON_Delete(root);
                }
            }
        }
        free(body);
    }

    if (url[0] == '\0')
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing url");
        return ESP_FAIL;
    }

    err = content_delete(url);
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t admin_media_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *name;
    const char *ext;
    const char *ctype = "application/octet-stream";
    char path[ADMIN_PATH_MAX];
    uint8_t buf[1024];
    FILE *fp;
    size_t n;
    esp_err_t err;

    if (uri == NULL || strncmp(uri, "/media/", 7) != 0)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    name = uri + 7;
    if (name[0] == '\0' || strchr(name, '/') != NULL || strstr(name, "..") != NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
        return ESP_FAIL;
    }

    ext = strrchr(name, '.');
    if (ext != NULL)
    {
        if (strcasecmp(ext, ".mp3") == 0)
        {
            ctype = "audio/mpeg";
        }
        else if (strcasecmp(ext, ".wav") == 0)
        {
            ctype = "audio/wav";
        }
        else if (strcasecmp(ext, ".ogg") == 0 || strcasecmp(ext, ".opus") == 0)
        {
            ctype = "audio/ogg";
        }
        else if (strcasecmp(ext, ".aac") == 0)
        {
            ctype = "audio/aac";
        }
        else if (strcasecmp(ext, ".img") == 0)
        {
            ctype = "application/octet-stream";
        }
    }

    snprintf(path, sizeof(path), ADMIN_MEDIA_DIR "/%s", name);
    fp = fopen(path, "rb");
    if (fp == NULL)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, ctype);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    {
        err = httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)n);
        if (err != ESP_OK)
        {
            fclose(fp);
            return err;
        }
    }
    fclose(fp);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t admin_register_handlers(httpd_handle_t server)
{
    static const httpd_uri_t root_uri =
    {
        .uri = "/",
        .method = HTTP_GET,
        .handler = admin_root_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t media_uri =
    {
        .uri = "/media/*",
        .method = HTTP_GET,
        .handler = admin_media_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t list_uri =
    {
        .uri = "/api/list",
        .method = HTTP_GET,
        .handler = admin_list_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t add_uri =
    {
        .uri = "/api/add",
        .method = HTTP_POST,
        .handler = admin_add_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t delete_uri =
    {
        .uri = "/api/delete",
        .method = HTTP_POST,
        .handler = admin_delete_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t login_uri =
    {
        .uri = "/api/login",
        .method = HTTP_POST,
        .handler = admin_login_handler,
        .user_ctx = NULL,
    };
    esp_err_t err;

    err = httpd_register_uri_handler(server, &root_uri);
    if (err != ESP_OK)
    {
        return err;
    }
    err = httpd_register_uri_handler(server, &media_uri);
    if (err != ESP_OK)
    {
        return err;
    }
    err = httpd_register_uri_handler(server, &list_uri);
    if (err != ESP_OK)
    {
        return err;
    }
    err = httpd_register_uri_handler(server, &add_uri);
    if (err != ESP_OK)
    {
        return err;
    }
    err = httpd_register_uri_handler(server, &login_uri);
    if (err != ESP_OK)
    {
        return err;
    }
    return httpd_register_uri_handler(server, &delete_uri);
}

/* ------------------------------------------------------------ lifecycle -- */

/** Tear down whatever parts of the admin session were brought up. */
static void admin_teardown(void)
{
    if (s_server != NULL)
    {
        httpd_stop(s_server);
        s_server = NULL;
    }
    if (s_wifi_started)
    {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_inited)
    {
        esp_wifi_deinit();
        s_wifi_inited = false;
    }
    if (s_ap_netif != NULL)
    {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    s_code = 0;
    s_code_str[0] = '\0';
    s_session_token[0] = '\0';
    s_active = false;
}

/** Generate a fresh 4-digit code, store it, and surface it via the callback. */
static void admin_new_code(void)
{
    s_code = (uint16_t)(esp_random() % 10000);
    snprintf(s_code_str, sizeof(s_code_str), "%04u", (unsigned)s_code);
    ESP_LOGI(TAG, "admin access code: %s", s_code_str);
    if (s_code_cb != NULL)
    {
        s_code_cb(s_code);
    }
}

esp_err_t admin_start(uint16_t *code_out)
{
    esp_err_t err;
    esp_netif_ip_info_t ip_info;
    wifi_init_config_t wifi_cfg;
    wifi_config_t ap_cfg;
    httpd_config_t httpd_cfg;

    if (s_active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_session_token[0] = '\0';

    /* The write path needs the SD card and mapping.json; content_init() is
     * safe to call again if the application already mounted the store. */
    err = content_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "content_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* One-time network stack bring-up, shared across start/stop cycles. */
    if (!s_netif_ready)
    {
        err = esp_netif_init();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
            return err;
        }
        err = esp_event_loop_create_default();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
            return err;
        }
        s_netif_ready = true;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL)
    {
        ESP_LOGE(TAG, "create AP netif failed");
        admin_teardown();
        return ESP_FAIL;
    }

    memset(&ip_info, 0, sizeof(ip_info));
    ip_info.ip.addr = esp_ip4addr_aton("192.168.4.1");
    ip_info.gw.addr = esp_ip4addr_aton("192.168.4.1");
    ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    esp_netif_dhcps_stop(s_ap_netif);
    err = esp_netif_set_ip_info(s_ap_netif, &ip_info);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set AP IP failed: %s", esp_err_to_name(err));
        admin_teardown();
        return err;
    }
    esp_netif_dhcps_start(s_ap_netif);

    wifi_cfg = (wifi_init_config_t)WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        admin_teardown();
        return err;
    }
    s_wifi_inited = true;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set AP mode failed: %s", esp_err_to_name(err));
        admin_teardown();
        return err;
    }

    memset(&ap_cfg, 0, sizeof(ap_cfg));
    strncpy((char *)ap_cfg.ap.ssid, ADMIN_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ADMIN_SSID);
    ap_cfg.ap.channel = ADMIN_CHANNEL;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = ADMIN_MAX_CONN;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set AP config failed: %s", esp_err_to_name(err));
        admin_teardown();
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        admin_teardown();
        return err;
    }
    s_wifi_started = true;

    httpd_cfg = (httpd_config_t)HTTPD_DEFAULT_CONFIG();
    httpd_cfg.max_uri_handlers = 8;
    httpd_cfg.max_open_sockets = 4;
    err = httpd_start(&s_server, &httpd_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        admin_teardown();
        return err;
    }

    err = admin_register_handlers(s_server);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "register handlers failed: %s", esp_err_to_name(err));
        admin_teardown();
        return err;
    }

    admin_new_code();
    s_active = true;

    if (code_out != NULL)
    {
        *code_out = s_code;
    }

    ESP_LOGI(TAG, "admin mode active (SSID=%s, http://192.168.4.1/)", ADMIN_SSID);
    return ESP_OK;
}

esp_err_t admin_stop(void)
{
    if (!s_active)
    {
        return ESP_OK;
    }
    admin_teardown();
    ESP_LOGI(TAG, "admin mode stopped");
    return ESP_OK;
}

bool admin_is_active(void)
{
    return s_active;
}

void admin_set_code_callback(admin_code_cb_t cb)
{
    s_code_cb = cb;
}
