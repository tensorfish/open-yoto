/*
 * admin.c — On-demand SoftAP, authenticated web UI, remote control, and SD
 * file management.
 *
 * admin_start() mounts content, starts the open `openyoto` AP and HTTP server,
 * then generates a six-character alphanumeric access code. POST /api/login
 * exchanges the code for an HttpOnly session cookie. All content, control, and
 * filesystem APIs require that cookie.
 */
#include "admin.h"
#include "content.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "admin";

static void admin_log_heap(const char *where)
{
    uint32_t free_internal = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t largest_internal = (uint32_t)heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "heap %s: internal free=%lu largest=%lu",
             where, (unsigned long)free_internal,
             (unsigned long)largest_internal);
}

static esp_err_t admin_send_oom(httpd_req_t *req, const char *where)
{
    admin_log_heap(where);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "out of memory");
    return ESP_FAIL;
}

/* SoftAP parameters (open network, fixed channel). */
#define ADMIN_SSID              "openyoto"
#define ADMIN_CHANNEL           1
#define ADMIN_MAX_CONN          4
#define ADMIN_COOKIE_NAME       "yoto_session"

/* Content store layout owned jointly with the content component. */
#define ADMIN_MEDIA_DIR         "/sdcard/media"
#define ADMIN_PATH_MAX          256
#define ADMIN_NAME_MAX          128
#define ADMIN_URL_MAX           512
#define ADMIN_BODY_MAX          (4 * 1024 * 1024)
#define ADMIN_FS_UPLOAD_CHUNK_SIZE 8192
#define ADMIN_MAX_TRACKS        32
#define ADMIN_MAX_FILES         64
#define ADMIN_MANIFEST_MAX      4096
#define ADMIN_BOUNDARY_MAX      128
#define ADMIN_CARD_UID_MAX      10
#define ADMIN_CARD_URL_MAX      200
#define ADMIN_CARD_BODY_MAX     1024

/* Active-session state. */
static bool             s_active;
static bool             s_netif_ready;
static bool             s_wifi_inited;
static bool             s_wifi_started;
static char             s_code_str[ADMIN_ACCESS_CODE_LEN + 1];
static char             s_session_token[33];
static httpd_handle_t   s_server;
static esp_netif_t     *s_ap_netif;
static admin_code_cb_t  s_code_cb;
static admin_path_cb_t  s_play_sound_cb;
static admin_path_cb_t  s_display_image_cb;
static admin_action_cb_t s_stop_sound_cb;
static admin_action_cb_t s_clear_display_cb;
static admin_card_write_cb_t s_write_card_cb;

/* Most recently captured NFC card, guarded by s_last_card_lock. */
static char s_last_card_url[ADMIN_CARD_URL_MAX + 1];
static portMUX_TYPE s_last_card_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_last_card_seq;
static uint8_t s_last_card_uid[ADMIN_CARD_UID_MAX];
static uint8_t s_last_card_uid_len;
static bool s_last_card_captured;

/* ------------------------------------------------------------------ page -- */

/* The single-page admin UI is embedded via ESP-IDF EMBED_FILES (see
 * CMakeLists.txt); the linker exposes it as these byte symbols. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* One uploaded file part: data points into the request body (kept alive for
 * the duration of the add request). */
typedef struct
{
    char             name[ADMIN_NAME_MAX];  /* original basename */
    const uint8_t   *data;
    size_t           len;
    char             ext[16];
    bool             is_image;
} admin_file_t;

/*
 * A parsed add request. Legacy single-file adds and multipart playlist
 * uploads retain their wire contracts. JSON playlists reference media already
 * uploaded through the streaming filesystem API.
 */
typedef struct
{
    char             url[ADMIN_URL_MAX];
    char             name[ADMIN_NAME_MAX];

    /* Single-add (legacy): spec strings (copied) or raw bytes (into body). */
    char             sound_spec[ADMIN_URL_MAX];
    char             image_spec[ADMIN_URL_MAX];
    const uint8_t   *sound_data;
    size_t           sound_len;
    char             sound_ext[16];
    const uint8_t   *image_data;
    size_t           image_len;
    char             image_ext[16];

    /* Playlist: ordered paths and optional parallel image paths. */
    struct
    {
        char         sound[ADMIN_NAME_MAX];
        char         image[ADMIN_NAME_MAX];
    } manifest[ADMIN_MAX_TRACKS];
    int              manifest_count;
    bool             playlist_refs;
    char             cover_image[ADMIN_NAME_MAX];
    admin_file_t     files[ADMIN_MAX_FILES];
    int              file_count;

    /* Owning pointer to the multipart body (NULL for JSON); freed by the
     * handler after ingest. */
    char            *body;
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
    if (strstr(mime, "audio/mp4") != NULL
        || strstr(mime, "audio/m4a") != NULL)
    {
        snprintf(ext, ext_len, ".m4a");
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
    if (mkdir(ADMIN_MEDIA_DIR, 0755) != 0 && errno != EEXIST)
    {
        ESP_LOGE(TAG, "cannot create %s", ADMIN_MEDIA_DIR);
        return ESP_FAIL;
    }

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

/* Parse a manifest JSON array into out->manifest (ordered [{"s","i"},...]). */
static esp_err_t admin_parse_manifest(const char *json, admin_add_req_t *out)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *item;
    int i = 0;

    if (root == NULL || !cJSON_IsArray(root))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    out->manifest_count = 0;
    cJSON_ArrayForEach(item, root)
    {
        cJSON *s;
        cJSON *im;

        if (i >= ADMIN_MAX_TRACKS)
        {
            break;
        }
        s = cJSON_GetObjectItemCaseSensitive(item, "s");
        im = cJSON_GetObjectItemCaseSensitive(item, "i");
        if (s != NULL && cJSON_IsString(s))
        {
            snprintf(out->manifest[i].sound, sizeof(out->manifest[i].sound),
                     "%s", s->valuestring);
        }
        if (im != NULL && cJSON_IsString(im))
        {
            snprintf(out->manifest[i].image, sizeof(out->manifest[i].image),
                     "%s", im->valuestring);
        }
        i++;
    }
    out->manifest_count = i;
    cJSON_Delete(root);
    return ESP_OK;
}

/* Find an uploaded file part by original basename and kind. */
static const admin_file_t *admin_find_file(const admin_add_req_t *add,
                                           const char *name, bool is_image)
{
    int i;

    if (name == NULL || name[0] == '\0')
    {
        return NULL;
    }
    for (i = 0; i < add->file_count; i++)
    {
        if (add->files[i].is_image == is_image
            && strcmp(add->files[i].name, name) == 0)
        {
            return &add->files[i];
        }
    }
    return NULL;
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
        else if (strcmp(name, "manifest") == 0)
        {
            char manifest_buf[ADMIN_MANIFEST_MAX];
            size_t copy = (part_len < sizeof(manifest_buf) - 1)
                        ? part_len : sizeof(manifest_buf) - 1;

            memcpy(manifest_buf, data, copy);
            manifest_buf[copy] = '\0';
            out->playlist_refs = true;
            if (admin_parse_manifest(manifest_buf, out) != ESP_OK)
            {
                return ESP_ERR_INVALID_ARG;
            }
        }
        else if (strcmp(name, "sound") == 0 || strcmp(name, "image") == 0)
        {
            bool is_image = (strcmp(name, "image") == 0);
            char media_ext[16];

            /* Derive the extension once so the sound kind can be validated. */
            admin_media_ext(media_ext, sizeof(media_ext), filename, mime,
                            is_image ? ".img" : ".mp3");

            /* Match the stock decoder set exposed by this replacement. */
            if (!is_image
                && strcasecmp(media_ext, ".mp3") != 0
                && strcasecmp(media_ext, ".m4a") != 0
                && strcasecmp(media_ext, ".aac") != 0)
            {
                ESP_LOGE(TAG, "unsupported audio type %s", media_ext);
                return ESP_ERR_INVALID_ARG;
            }

            /* Record the first part of each kind for the single-add path. */
            if (!is_image && out->sound_data == NULL)
            {
                out->sound_data = (const uint8_t *)data;
                out->sound_len = part_len;
                snprintf(out->sound_ext, sizeof(out->sound_ext), "%s", media_ext);
            }
            else if (is_image && out->image_data == NULL)
            {
                out->image_data = (const uint8_t *)data;
                out->image_len = part_len;
                snprintf(out->image_ext, sizeof(out->image_ext), "%s", media_ext);
            }

            /* Collect every part for the playlist path. */
            if (out->file_count < ADMIN_MAX_FILES)
            {
                admin_file_t *f = &out->files[out->file_count];

                admin_copy_field(f->name, sizeof(f->name), filename, strlen(filename));
                f->data = (const uint8_t *)data;
                f->len = part_len;
                snprintf(f->ext, sizeof(f->ext), "%s", media_ext);
                f->is_image = is_image;
                out->file_count++;
            }
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

/*
 * Parse the JSON playlist contract:
 * {"url":"...","name":"...","tracks":["media/a.mp3"],
 *  "track_images":["media/a.img"],"image":"media/cover.img"}.
 *
 * Legacy JSON {"url","sound","image"} remains supported on /api/add.
 */
static esp_err_t admin_parse_json_add(const char *body, admin_add_req_t *out)
{
    cJSON *root = cJSON_Parse(body);
    cJSON *tracks;
    cJSON *images;
    cJSON *item;
    int count;
    int i;

    if (root == NULL || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (item == NULL || !cJSON_IsString(item) || item->valuestring == NULL
        || item->valuestring[0] == '\0'
        || strlen(item->valuestring) >= sizeof(out->url))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(out->url, sizeof(out->url), "%s", item->valuestring);

    tracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    out->playlist_refs = tracks != NULL;

    item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (item != NULL)
    {
        if (!cJSON_IsString(item) || item->valuestring == NULL
            || strlen(item->valuestring) >= sizeof(out->name))
        {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(out->name, sizeof(out->name), "%s", item->valuestring);
    }

    if (tracks == NULL)
    {
        item = cJSON_GetObjectItemCaseSensitive(root, "sound");
        if (item != NULL && cJSON_IsString(item) && item->valuestring != NULL)
        {
            snprintf(out->sound_spec, sizeof(out->sound_spec), "%s",
                     item->valuestring);
        }
        item = cJSON_GetObjectItemCaseSensitive(root, "image");
        if (item != NULL && cJSON_IsString(item) && item->valuestring != NULL)
        {
            snprintf(out->image_spec, sizeof(out->image_spec), "%s",
                     item->valuestring);
        }
        cJSON_Delete(root);
        return ESP_OK;
    }

    images = cJSON_GetObjectItemCaseSensitive(root, "track_images");
    if (!cJSON_IsArray(tracks) || !cJSON_IsArray(images))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    count = cJSON_GetArraySize(tracks);
    if (count <= 0 || count > ADMIN_MAX_TRACKS
        || cJSON_GetArraySize(images) != count)
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    for (i = 0; i < count; i++)
    {
        cJSON *sound = cJSON_GetArrayItem(tracks, i);
        cJSON *image = cJSON_GetArrayItem(images, i);

        if (sound == NULL || !cJSON_IsString(sound) || sound->valuestring == NULL
            || sound->valuestring[0] == '\0'
            || strlen(sound->valuestring) >= sizeof(out->manifest[i].sound)
            || image == NULL || !cJSON_IsString(image)
            || image->valuestring == NULL
            || strlen(image->valuestring) >= sizeof(out->manifest[i].image))
        {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(out->manifest[i].sound, sizeof(out->manifest[i].sound), "%s",
                 sound->valuestring);
        snprintf(out->manifest[i].image, sizeof(out->manifest[i].image), "%s",
                 image->valuestring);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "image");
    if (item != NULL)
    {
        if (!cJSON_IsString(item) || item->valuestring == NULL
            || strlen(item->valuestring) >= sizeof(out->cover_image))
        {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(out->cover_image, sizeof(out->cover_image), "%s",
                 item->valuestring);
    }
    out->manifest_count = count;
    out->playlist_refs = true;
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
        if (err != ESP_OK)
        {
            free(body);
            return err;
        }
        out->body = body;   /* kept alive for ingest; the handler frees it */
        return ESP_OK;
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
    else if (add->sound_spec[0] != '\0')
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
    else if (add->image_spec[0] != '\0')
    {
        err = admin_ingest_spec(add->image_spec, ".png", image_name, image_name_len);
    }
    return err;
}

/* Save a playlist's files and register a playlist mapping entry. */
static esp_err_t admin_ingest_playlist(const admin_add_req_t *add)
{
    char tracks[ADMIN_MAX_TRACKS][ADMIN_NAME_MAX];
    char track_images[ADMIN_MAX_TRACKS][ADMIN_NAME_MAX];
    const char *track_ptrs[ADMIN_MAX_TRACKS];
    const char *image_ptrs[ADMIN_MAX_TRACKS];
    char cover[ADMIN_NAME_MAX] = { 0 };
    int i;

    if (add->manifest_count <= 0 || add->manifest_count > ADMIN_MAX_TRACKS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (i = 0; i < add->manifest_count; i++)
    {
        const admin_file_t *sf;
        const admin_file_t *ifile;

        sf = admin_find_file(add, add->manifest[i].sound, false);
        ifile = admin_find_file(add, add->manifest[i].image, true);

        if (sf == NULL)
        {
            ESP_LOGE(TAG, "manifest references missing sound %s",
                     add->manifest[i].sound);
            return ESP_ERR_NOT_FOUND;
        }
        if (admin_save_media(sf->data, sf->len, sf->ext,
                             tracks[i], sizeof(tracks[i])) != ESP_OK)
        {
            return ESP_FAIL;
        }

        if (ifile != NULL)
        {
            if (admin_save_media(ifile->data, ifile->len, ifile->ext,
                                 track_images[i], sizeof(track_images[i])) != ESP_OK)
            {
                return ESP_FAIL;
            }
        }
        else
        {
            track_images[i][0] = '\0';
        }
    }

    /* Use the first track's image as the card cover (list thumbnail). */
    if (track_images[0][0] != '\0')
    {
        snprintf(cover, sizeof(cover), "%s", track_images[0]);
    }

    for (i = 0; i < add->manifest_count; i++)
    {
        track_ptrs[i] = tracks[i];
        image_ptrs[i] = track_images[i];
    }

    return content_add_playlist(add->url, add->name, track_ptrs, image_ptrs,
                                add->manifest_count,
                                cover[0] != '\0' ? cover : NULL);
}


/* ----------------------------------------------------------------- auth -- */

/** Return true when `pin` matches the active six-character code. */
static bool admin_pin_match(const char *pin)
{
    return s_code_str[0] != '\0' && pin != NULL
        && strlen(pin) == ADMIN_ACCESS_CODE_LEN
        && strcmp(pin, s_code_str) == 0;
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

static bool admin_require_session(httpd_req_t *req)
{
    if (admin_session_ok(req))
    {
        return true;
    }
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
    return false;
}

static int admin_hex_digit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

static bool admin_url_decode(char *value)
{
    char *src = value;
    char *dst = value;

    while (*src != '\0')
    {
        if (*src == '%' && src[1] != '\0' && src[2] != '\0')
        {
            int hi = admin_hex_digit(src[1]);
            int lo = admin_hex_digit(src[2]);
            if (hi < 0 || lo < 0)
            {
                return false;
            }
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
        }
        else
        {
            *dst++ = (*src == '+') ? ' ' : *src;
            src++;
        }
    }
    *dst = '\0';
    return true;
}

static bool admin_path_segments_safe(const char *path)
{
    const char *p = path;

    while (*p != '\0')
    {
        const char *segment;
        size_t len;

        while (*p == '/')
        {
            p++;
        }
        segment = p;
        while (*p != '\0' && *p != '/')
        {
            unsigned char c = (unsigned char)*p;
            if (c < 0x20 || c == '\\')
            {
                return false;
            }
            p++;
        }
        len = (size_t)(p - segment);
        if ((len == 1 && segment[0] == '.')
            || (len == 2 && segment[0] == '.' && segment[1] == '.'))
        {
            return false;
        }
    }
    return true;
}

/** Validate and copy an explicit path rooted at /sdcard. */
static bool admin_resolve_sd_path(const char *input, char *out, size_t out_len)
{
    size_t mount_len = strlen(CONTENT_MOUNT_POINT);
    const char *relative;
    int written;

    if (input == NULL || out == NULL || out_len == 0
        || strncmp(input, CONTENT_MOUNT_POINT, mount_len) != 0
        || (input[mount_len] != '\0' && input[mount_len] != '/'))
    {
        return false;
    }
    relative = input + mount_len;
    if (!admin_path_segments_safe(relative))
    {
        return false;
    }
    written = snprintf(out, out_len, "%s", input);
    return written > 0 && (size_t)written < out_len;
}

static bool admin_query_path(httpd_req_t *req, const char *key,
                             char *out, size_t out_len)
{
    char query[ADMIN_URL_MAX];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK
        || httpd_query_key_value(query, key, out, out_len) != ESP_OK)
    {
        return false;
    }
    return admin_url_decode(out);
}

static esp_err_t admin_send_fs_errno(httpd_req_t *req, const char *operation,
                                     const char *path, int error_number)
{
    const char *status = "500 Internal Server Error";
    const char *message = "filesystem operation failed";

    ESP_LOGE(TAG, "%s %s failed: errno=%d (%s)", operation, path,
             error_number, strerror(error_number));
    if (error_number == ENOMEM || error_number == EMFILE
        || error_number == ENFILE)
    {
        admin_log_heap(operation);
    }

    switch (error_number)
    {
        case EINVAL:
        case ENAMETOOLONG:
            status = "400 Bad Request";
            message = "invalid path";
            break;
        case EACCES:
        case EPERM:
        case EROFS:
            status = "403 Forbidden";
            message = "SD card is read-only";
            break;
        case ENOENT:
            status = "404 Not Found";
            message = "parent directory not found";
            break;
        case EEXIST:
            status = "409 Conflict";
            message = "path already exists";
            break;
        case ENOSPC:
            status = "507 Insufficient Storage";
            message = "SD card is full";
            break;
        case ENOMEM:
        case EMFILE:
        case ENFILE:
            message = "not enough memory or file handles";
            break;
        default:
            break;
    }

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
}

static int admin_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f')
    {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F')
    {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool admin_parse_uid(const char *hex, uint8_t *uid, uint8_t *uid_len)
{
    size_t len = hex == NULL ? 0 : strlen(hex);

    if (len == 0 || (len & 1) != 0 || len / 2 > ADMIN_CARD_UID_MAX)
    {
        return false;
    }
    for (size_t i = 0; i < len / 2; i++)
    {
        int high = admin_hex_value(hex[i * 2]);
        int low = admin_hex_value(hex[i * 2 + 1]);

        if (high < 0 || low < 0)
        {
            return false;
        }
        uid[i] = (uint8_t)((high << 4) | low);
    }
    *uid_len = (uint8_t)(len / 2);
    return true;
}

static void admin_format_uid(const uint8_t *uid, uint8_t uid_len,
                             char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";

    if (out_len < (size_t)uid_len * 2 + 1)
    {
        if (out_len > 0)
        {
            out[0] = '\0';
        }
        return;
    }
    for (uint8_t i = 0; i < uid_len; i++)
    {
        out[i * 2] = hex[uid[i] >> 4];
        out[i * 2 + 1] = hex[uid[i] & 0x0F];
    }
    out[uid_len * 2] = '\0';
}

static cJSON *admin_read_json(httpd_req_t *req)
{
    char *body = NULL;
    size_t body_len = 0;
    cJSON *root;

    if (admin_read_body(req, &body, &body_len) != ESP_OK)
    {
        return NULL;
    }
    root = cJSON_ParseWithLength(body, body_len);
    free(body);
    return root;
}

static bool admin_json_string(cJSON *root, const char *key,
                              char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (item == NULL || !cJSON_IsString(item) || item->valuestring == NULL
        || item->valuestring[0] == '\0')
    {
        return false;
    }
    return snprintf(out, out_len, "%s", item->valuestring) > 0
        && strlen(item->valuestring) < out_len;
}

/*
 * Playlist JSON contains catalog-relative paths. Resolve them against the SD
 * mount, require a regular uploaded file, and constrain the media type before
 * the catalog is changed.
 */
static bool admin_playlist_media_path(const char *path, bool audio,
                                      char *absolute, size_t absolute_len)
{
    char candidate[ADMIN_PATH_MAX];
    const char *ext;
    struct stat st;

    if (path == NULL
        || strncmp(path, "media/", strlen("media/")) != 0
        || path[strlen("media/")] == '\0'
        || snprintf(candidate, sizeof(candidate), "%s/%s",
                    CONTENT_MOUNT_POINT, path) >= (int)sizeof(candidate)
        || !admin_resolve_sd_path(candidate, absolute, absolute_len)
        || stat(absolute, &st) != 0 || !S_ISREG(st.st_mode))
    {
        return false;
    }
    ext = strrchr(path, '.');
    if (ext == NULL)
    {
        return false;
    }
    if (audio)
    {
        return strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".m4a") == 0
            || strcasecmp(ext, ".aac") == 0;
    }
    return strcasecmp(ext, ".img") == 0;
}

static esp_err_t admin_register_playlist(const admin_add_req_t *add)
{
    const char *tracks[ADMIN_MAX_TRACKS];
    const char *images[ADMIN_MAX_TRACKS];
    const char *cover = NULL;
    char absolute[ADMIN_PATH_MAX];
    int i;

    if (add->manifest_count <= 0 || add->manifest_count > ADMIN_MAX_TRACKS)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (i = 0; i < add->manifest_count; i++)
    {
        if (!admin_playlist_media_path(add->manifest[i].sound, true, absolute,
                                       sizeof(absolute))
            || (add->manifest[i].image[0] != '\0'
                && !admin_playlist_media_path(add->manifest[i].image, false,
                                              absolute, sizeof(absolute))))
        {
            return ESP_ERR_INVALID_ARG;
        }
        tracks[i] = add->manifest[i].sound;
        images[i] = add->manifest[i].image;
        if (cover == NULL && images[i][0] != '\0')
        {
            cover = images[i];
        }
    }
    if (add->cover_image[0] != '\0')
    {
        if (!admin_playlist_media_path(add->cover_image, false, absolute,
                                       sizeof(absolute)))
        {
            return ESP_ERR_INVALID_ARG;
        }
        cover = add->cover_image;
    }
    return content_add_playlist(add->url, add->name, tracks, images,
                                add->manifest_count, cover);
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
    char cookie[96];
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
             ADMIN_COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict",
             s_session_token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t admin_list_handler(httpd_req_t *req)
{
    if (!admin_require_session(req))
    {
        return ESP_FAIL;
    }

    char *json = NULL;
    esp_err_t err = content_list_json(&json);

    if (err != ESP_OK)
    {
        if (err == ESP_ERR_NO_MEM)
        {
            return admin_send_oom(req, "serializing card library");
        }
        {
            char message[96];
            snprintf(message, sizeof(message), "library load failed: %s",
                     esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, message);
        }
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return err;
}

/* Return the most recently captured card, including blank cards by UID. */
static esp_err_t admin_last_card_handler(httpd_req_t *req)
{
    char url[ADMIN_CARD_URL_MAX + 1];
    uint8_t uid[ADMIN_CARD_UID_MAX];
    uint8_t uid_len;
    char uid_hex[ADMIN_CARD_UID_MAX * 2 + 1];
    bool captured;
    bool exists;
    cJSON *root;
    char *json;
    uint32_t seq;

    if (!admin_require_session(req))
    {
        return ESP_FAIL;
    }
    portENTER_CRITICAL(&s_last_card_lock);
    snprintf(url, sizeof(url), "%s", s_last_card_url);
    uid_len = s_last_card_uid_len;
    memcpy(uid, s_last_card_uid, uid_len);
    captured = s_last_card_captured;
    seq = s_last_card_seq;
    portEXIT_CRITICAL(&s_last_card_lock);

    admin_format_uid(uid, uid_len, uid_hex, sizeof(uid_hex));
    exists = captured && url[0] != '\0'
          && content_lookup(url, NULL, 0, NULL, 0) == ESP_OK;

    root = cJSON_CreateObject();
    if (root == NULL)
    {
        return admin_send_oom(req, "creating last-card response");
    }
    cJSON_AddBoolToObject(root, "captured", captured);
    cJSON_AddStringToObject(root, "uid", uid_hex);
    cJSON_AddStringToObject(root, "url", url);
    cJSON_AddBoolToObject(root, "exists", exists);
    cJSON_AddNumberToObject(root, "seq", (double)seq);

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL)
    {
        return admin_send_oom(req, "serializing last-card response");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t admin_card_write_handler(httpd_req_t *req)
{
    cJSON *root;
    cJSON *seq_item;
    char url[ADMIN_CARD_URL_MAX + 1];
    char uid_hex[ADMIN_CARD_UID_MAX * 2 + 1];
    uint8_t uid[ADMIN_CARD_UID_MAX];
    uint8_t uid_len;
    uint32_t result_seq;
    bool matches;
    esp_err_t err;

    if (!admin_require_session(req))
    {
        return ESP_FAIL;
    }
    if (req->content_len == 0 || req->content_len > ADMIN_CARD_BODY_MAX)
    {
        return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE,
                                   "card request too large");
    }
    root = admin_read_json(req);
    seq_item = root == NULL ? NULL
             : cJSON_GetObjectItemCaseSensitive(root, "seq");
    if (root == NULL
        || !admin_json_string(root, "url", url, sizeof(url))
        || !admin_json_string(root, "uid", uid_hex, sizeof(uid_hex))
        || seq_item == NULL || !cJSON_IsNumber(seq_item)
        || seq_item->valuedouble < 0
        || seq_item->valuedouble > UINT32_MAX
        || !admin_parse_uid(uid_hex, uid, &uid_len))
    {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid card write request");
    }
    cJSON_Delete(root);

    portENTER_CRITICAL(&s_last_card_lock);
    matches = s_last_card_captured
           && s_last_card_uid_len == uid_len
           && memcmp(s_last_card_uid, uid, uid_len) == 0;
    portEXIT_CRITICAL(&s_last_card_lock);
    if (!matches)
    {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "captured card UID changed; scan again");
    }
    if (s_write_card_cb == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "card writer unavailable");
    }

    err = s_write_card_cb(url, uid, uid_len);
    if (err != ESP_OK)
    {
        if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE)
        {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain");
            return httpd_resp_sendstr(
                req, err == ESP_ERR_NOT_FOUND
                   ? "no card present" : "different card present");
        }
        if (err == ESP_ERR_INVALID_SIZE)
        {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain");
            return httpd_resp_sendstr(req,
                                      "URL does not fit card user area");
        }
        if (err == ESP_ERR_NOT_ALLOWED)
        {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain");
            return httpd_resp_sendstr(req,
                                      "target card user page is permanently locked");
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "card write or verification failed");
    }

    portENTER_CRITICAL(&s_last_card_lock);
    if (s_last_card_uid_len == uid_len
        && memcmp(s_last_card_uid, uid, uid_len) == 0)
    {
        snprintf(s_last_card_url, sizeof(s_last_card_url), "%s", url);
        s_last_card_seq++;
    }
    result_seq = s_last_card_seq;
    portEXIT_CRITICAL(&s_last_card_lock);

    char response[64];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"seq\":%lu}", (unsigned long)result_seq);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t admin_add_handler(httpd_req_t *req)
{
    admin_add_req_t *add;
    char ct[256] = { 0 };
    size_t ct_len;
    esp_err_t err;
    char sound_name[ADMIN_NAME_MAX] = { 0 };
    char image_name[ADMIN_NAME_MAX] = { 0 };

    bool playlist_refs;
    bool is_playlist;
    if (!admin_session_ok(req))
    {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
        return ESP_FAIL;
    }

    /* The request struct is ~20 KB (manifest + file tables); keep it off the
     * httpd task's stack. */
    add = calloc(1, sizeof(*add));
    if (add == NULL)
    {
        return admin_send_oom(req, "allocating add-content request");
    }

    ct_len = httpd_req_get_hdr_value_len(req, "Content-Type");
    if (ct_len > 0 && ct_len < sizeof(ct))
    {
        httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));
    }

    err = admin_parse_add_request(req, ct, add);
    if (err != ESP_OK)
    {
        bool invalid_playlist = add->playlist_refs;

        free(add->body);
        free(add);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            invalid_playlist ? "invalid playlist"
                                              : "bad request");
        return ESP_FAIL;
    }
    if (add->url[0] == '\0')
    {
        free(add->body);
        free(add);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing url");
        return ESP_FAIL;
    }
    if (add->name[0] == '\0')
    {
        snprintf(add->name, sizeof(add->name), "%.*s",
                 (int)sizeof(add->name) - 1, add->url);
    }

    playlist_refs = add->playlist_refs;
    is_playlist = playlist_refs || add->manifest_count > 0;
    if (playlist_refs)
    {
        err = admin_register_playlist(add);
    }
    else if (add->manifest_count > 0)
    {
        err = admin_ingest_playlist(add);
    }
    else
    {
        err = admin_ingest_add(add, sound_name, sizeof(sound_name),
                               image_name, sizeof(image_name));
        if (err == ESP_OK)
        {
            err = content_add(add->url, sound_name, image_name);
        }
    }
    free(add->body);
    free(add);

    if (err != ESP_OK)
    {
        if (is_playlist && (err == ESP_ERR_INVALID_ARG
                            || err == ESP_ERR_NOT_FOUND))
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "invalid playlist");
        }
        else if (is_playlist)
        {
            char message[96];

            snprintf(message, sizeof(message), "playlist save failed: %s",
                     esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, message);
        }
        else
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "add failed");
        }
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
            return admin_send_oom(req, "allocating delete request");
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
        char message[96];
        snprintf(message, sizeof(message), "delete failed: %s",
                 esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, message);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t admin_delete_all_handler(httpd_req_t *req)
{
    esp_err_t err;

    if (!admin_session_ok(req))
    {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
        return ESP_FAIL;
    }

    err = content_delete_all();
    if (err != ESP_OK)
    {
        char message[96];

        snprintf(message, sizeof(message), "delete all failed: %s",
                 esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, message);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t admin_media_handler(httpd_req_t *req)
{
    if (!admin_require_session(req))
    {
        return ESP_FAIL;
    }

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
        else if (strcasecmp(ext, ".m4a") == 0)
        {
            ctype = "audio/mp4";
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

static const char *admin_content_type(const char *path)
{
    const char *ext = strrchr(path, '.');

    if (ext == NULL)
    {
        return "application/octet-stream";
    }
    if (strcasecmp(ext, ".mp3") == 0)
    {
        return "audio/mpeg";
    }
    if (strcasecmp(ext, ".m4a") == 0)
    {
        return "audio/mp4";
    }
    if (strcasecmp(ext, ".aac") == 0)
    {
        return "audio/aac";
    }
    if (strcasecmp(ext, ".png") == 0)
    {
        return "image/png";
    }
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)
    {
        return "image/jpeg";
    }
    if (strcasecmp(ext, ".gif") == 0)
    {
        return "image/gif";
    }
    return "application/octet-stream";
}

static esp_err_t admin_control_handler(httpd_req_t *req,
                                       admin_path_cb_t callback)
{
    cJSON *root;
    char logical[ADMIN_PATH_MAX];
    char absolute[ADMIN_PATH_MAX];
    struct stat st;
    esp_err_t err;

    if (!admin_require_session(req))
    {
        return ESP_FAIL;
    }
    if (callback == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "control unavailable");
        return ESP_FAIL;
    }
    root = admin_read_json(req);
    if (root == NULL
        || !admin_json_string(root, "path", logical, sizeof(logical))
        || !admin_resolve_sd_path(logical, absolute, sizeof(absolute)))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }
    cJSON_Delete(root);
    if (stat(absolute, &st) != 0 || !S_ISREG(st.st_mode))
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }
    err = callback(absolute);
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t admin_play_handler(httpd_req_t *req)
{
    return admin_control_handler(req, s_play_sound_cb);
}

static esp_err_t admin_display_handler(httpd_req_t *req)
{
    return admin_control_handler(req, s_display_image_cb);
}

static esp_err_t admin_action_handler(httpd_req_t *req,
                                      admin_action_cb_t callback)
{
    esp_err_t err;

    if (!admin_require_session(req))
    {
        return ESP_FAIL;
    }
    if (callback == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "control unavailable");
        return ESP_FAIL;
    }
    err = callback();
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t admin_stop_sound_handler(httpd_req_t *req)
{
    return admin_action_handler(req, s_stop_sound_cb);
}

static esp_err_t admin_clear_display_handler(httpd_req_t *req)
{
    return admin_action_handler(req, s_clear_display_cb);
}

static esp_err_t admin_fs_list_handler(httpd_req_t *req)
{
    char logical[ADMIN_PATH_MAX];
    char absolute[ADMIN_PATH_MAX];
    DIR *dir;
    struct dirent *entry;
    bool first = true;
    esp_err_t err;

    if (!admin_require_session(req))
    {
        return ESP_FAIL;
    }
    if (!admin_query_path(req, "path", logical, sizeof(logical)))
    {
        snprintf(logical, sizeof(logical), CONTENT_MOUNT_POINT "/media");
    }
    if (!admin_resolve_sd_path(logical, absolute, sizeof(absolute)))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }
    dir = opendir(absolute);
    if (dir == NULL)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "directory not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send_chunk(req, "[", 1);
    while (err == ESP_OK && (entry = readdir(dir)) != NULL)
    {
        char child_abs[ADMIN_PATH_MAX];
        struct stat st;
        cJSON *item;
        char *json;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }
        if (snprintf(child_abs, sizeof(child_abs), "%s/%s",
                     absolute, entry->d_name) >= (int)sizeof(child_abs)
            || stat(child_abs, &st) != 0)
        {
            continue;
        }

        item = cJSON_CreateObject();
        if (item == NULL)
        {
            err = ESP_ERR_NO_MEM;
            break;
        }
        cJSON_AddStringToObject(item, "name", entry->d_name);
        cJSON_AddStringToObject(item, "path", child_abs);
        cJSON_AddStringToObject(item, "type",
                               S_ISDIR(st.st_mode) ? "directory" : "file");
        cJSON_AddNumberToObject(item, "size",
                                S_ISDIR(st.st_mode) ? 0 : (double)st.st_size);
        json = cJSON_PrintUnformatted(item);
        cJSON_Delete(item);
        if (json == NULL)
        {
            err = ESP_ERR_NO_MEM;
            break;
        }
        if (!first)
        {
            err = httpd_resp_send_chunk(req, ",", 1);
        }
        if (err == ESP_OK)
        {
            err = httpd_resp_send_chunk(req, json, HTTPD_RESP_USE_STRLEN);
        }
        cJSON_free(json);
        first = false;
    }
    closedir(dir);
    if (err != ESP_OK)
    {
        if (err == ESP_ERR_NO_MEM)
        {
            admin_log_heap("streaming directory listing");
        }
        ESP_LOGE(TAG, "directory response failed for %s: %s",
                 absolute, esp_err_to_name(err));
        return err;
    }
    err = httpd_resp_send_chunk(req, "]", 1);
    if (err == ESP_OK)
    {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t admin_fs_download_handler(httpd_req_t *req)
{
    char logical[ADMIN_PATH_MAX];
    char absolute[ADMIN_PATH_MAX];
    char disposition[ADMIN_PATH_MAX + 32];
    const char *name;
    FILE *fp;
    uint8_t buffer[2048];
    size_t count;
    esp_err_t err;

    if (!admin_require_session(req))
    {
        return ESP_FAIL;
    }
    if (!admin_query_path(req, "path", logical, sizeof(logical))
        || !admin_resolve_sd_path(logical, absolute, sizeof(absolute)))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }
    fp = fopen(absolute, "rb");
    if (fp == NULL)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }
    name = strrchr(absolute, '/');
    name = name == NULL ? absolute : name + 1;
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(req, admin_content_type(absolute));
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    while ((count = fread(buffer, 1, sizeof(buffer), fp)) > 0)
    {
        err = httpd_resp_send_chunk(req, (const char *)buffer, (ssize_t)count);
        if (err != ESP_OK)
        {
            fclose(fp);
            return err;
        }
    }
    fclose(fp);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t admin_fs_create_handler(httpd_req_t *req)
{
    cJSON *root;
    char logical[ADMIN_PATH_MAX];
    char absolute[ADMIN_PATH_MAX];
    int fd;

    if (!admin_require_session(req))
    {
        return ESP_FAIL;
    }
    root = admin_read_json(req);
    if (root == NULL
        || !admin_json_string(root, "path", logical, sizeof(logical))
        || !admin_resolve_sd_path(logical, absolute, sizeof(absolute))
        || strcmp(absolute, CONTENT_MOUNT_POINT) == 0)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }
    cJSON_Delete(root);

    fd = open(absolute, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
    {
        int error_number = errno;
        return admin_send_fs_errno(req, "create file", absolute, error_number);
    }
    if (close(fd) != 0)
    {
        int error_number = errno;
        unlink(absolute);
        return admin_send_fs_errno(req, "close created file", absolute,
                                   error_number);
    }

    ESP_LOGI(TAG, "created empty file %s", absolute);
    httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t admin_fs_upload_handler(httpd_req_t *req)
{
    char logical[ADMIN_PATH_MAX];
    char absolute[ADMIN_PATH_MAX];
    FILE *fp;
    uint8_t buffer[ADMIN_FS_UPLOAD_CHUNK_SIZE];
    size_t remaining = req->content_len;
    size_t written_total = 0;

    if (!admin_require_session(req))
    {
        return ESP_FAIL;
    }
    if (!admin_query_path(req, "path", logical, sizeof(logical))
        || !admin_resolve_sd_path(logical, absolute, sizeof(absolute))
        || strcmp(absolute, CONTENT_MOUNT_POINT) == 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "invalid upload path");
        return ESP_FAIL;
    }
    fp = fopen(absolute, "wb");
    if (fp == NULL)
    {
        int error_number = errno;
        return admin_send_fs_errno(req, "open upload", absolute, error_number);
    }
    while (remaining > 0)
    {
        size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        int got = httpd_req_recv(req, (char *)buffer, wanted);

        if (got <= 0 || fwrite(buffer, 1, (size_t)got, fp) != (size_t)got)
        {
            fclose(fp);
            unlink(absolute);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "upload failed while writing");
            return ESP_FAIL;
        }
        remaining -= (size_t)got;
        written_total += (size_t)got;
    }
    if (fclose(fp) != 0)
    {
        unlink(absolute);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "upload failed while closing");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "uploaded %s (%u bytes)", absolute, (unsigned)written_total);
    httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t admin_fs_mkdir_handler(httpd_req_t *req)
{
    cJSON *root;
    char logical[ADMIN_PATH_MAX];
    char absolute[ADMIN_PATH_MAX];
    struct stat st;
    bool created = false;

    if (!admin_require_session(req))
    {
        return ESP_FAIL;
    }
    root = admin_read_json(req);
    if (root == NULL
        || !admin_json_string(root, "path", logical, sizeof(logical))
        || !admin_resolve_sd_path(logical, absolute, sizeof(absolute))
        || strcmp(absolute, CONTENT_MOUNT_POINT) == 0)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }
    cJSON_Delete(root);
    if (mkdir(absolute, 0755) == 0)
    {
        created = true;
    }
    else if (errno != EEXIST || stat(absolute, &st) != 0
             || !S_ISDIR(st.st_mode))
    {
        int error_number = errno;
        return admin_send_fs_errno(req, "create directory", absolute,
                                   error_number);
    }
    httpd_resp_set_status(req, created ? "201 Created" : "200 OK");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t admin_fs_rename_handler(httpd_req_t *req)
{
    cJSON *root;
    char from[ADMIN_PATH_MAX];
    char to[ADMIN_PATH_MAX];
    char from_abs[ADMIN_PATH_MAX];
    char to_abs[ADMIN_PATH_MAX];

    if (!admin_require_session(req))
    {
        return ESP_FAIL;
    }
    root = admin_read_json(req);
    if (root == NULL
        || !admin_json_string(root, "from", from, sizeof(from))
        || !admin_json_string(root, "to", to, sizeof(to))
        || !admin_resolve_sd_path(from, from_abs, sizeof(from_abs))
        || !admin_resolve_sd_path(to, to_abs, sizeof(to_abs))
        || strcmp(from_abs, CONTENT_MOUNT_POINT) == 0
        || strcmp(to_abs, CONTENT_MOUNT_POINT) == 0)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }
    cJSON_Delete(root);
    if (rename(from_abs, to_abs) != 0)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "rename failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t admin_fs_delete_handler(httpd_req_t *req)
{
    cJSON *root;
    char logical[ADMIN_PATH_MAX];
    char absolute[ADMIN_PATH_MAX];
    struct stat st;
    int rc;

    if (!admin_require_session(req))
    {
        return ESP_FAIL;
    }
    root = admin_read_json(req);
    if (root == NULL
        || !admin_json_string(root, "path", logical, sizeof(logical))
        || !admin_resolve_sd_path(logical, absolute, sizeof(absolute))
        || strcmp(absolute, CONTENT_MOUNT_POINT) == 0)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }
    cJSON_Delete(root);
    if (stat(absolute, &st) != 0)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "path not found");
        return ESP_FAIL;
    }
    rc = S_ISDIR(st.st_mode) ? rmdir(absolute) : unlink(absolute);
    if (rc != 0)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            S_ISDIR(st.st_mode)
                                ? "directory must be empty" : "delete failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t admin_register_handlers(httpd_handle_t server)
{
    static const httpd_uri_t handlers[] =
    {
        { .uri = "/", .method = HTTP_GET,
          .handler = admin_root_handler },
        { .uri = "/media/*", .method = HTTP_GET,
          .handler = admin_media_handler },
        { .uri = "/api/login", .method = HTTP_POST,
          .handler = admin_login_handler },
        { .uri = "/api/list", .method = HTTP_GET,
          .handler = admin_list_handler },
        { .uri = "/api/last-card", .method = HTTP_GET,
          .handler = admin_last_card_handler },
        { .uri = "/api/card/write", .method = HTTP_POST,
          .handler = admin_card_write_handler },
        { .uri = "/api/add", .method = HTTP_POST,
          .handler = admin_add_handler },
        { .uri = "/api/delete", .method = HTTP_POST,
          .handler = admin_delete_handler },
        { .uri = "/api/delete-all", .method = HTTP_POST,
          .handler = admin_delete_all_handler },
        { .uri = "/api/control/play", .method = HTTP_POST,
          .handler = admin_play_handler },
        { .uri = "/api/control/display", .method = HTTP_POST,
          .handler = admin_display_handler },
        { .uri = "/api/control/stop", .method = HTTP_POST,
          .handler = admin_stop_sound_handler },
        { .uri = "/api/control/clear", .method = HTTP_POST,
          .handler = admin_clear_display_handler },
        { .uri = "/api/fs/list", .method = HTTP_GET,
          .handler = admin_fs_list_handler },
        { .uri = "/api/fs/file", .method = HTTP_GET,
          .handler = admin_fs_download_handler },
        { .uri = "/api/fs/upload", .method = HTTP_POST,
          .handler = admin_fs_upload_handler },
        { .uri = "/api/fs/create", .method = HTTP_POST,
          .handler = admin_fs_create_handler },
        { .uri = "/api/fs/mkdir", .method = HTTP_POST,
          .handler = admin_fs_mkdir_handler },
        { .uri = "/api/fs/rename", .method = HTTP_POST,
          .handler = admin_fs_rename_handler },
        { .uri = "/api/fs/delete", .method = HTTP_POST,
          .handler = admin_fs_delete_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++)
    {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    return ESP_OK;
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
    s_code_str[0] = '\0';
    s_session_token[0] = '\0';
    portENTER_CRITICAL(&s_last_card_lock);
    s_last_card_url[0] = '\0';
    s_last_card_uid_len = 0;
    s_last_card_captured = false;
    s_last_card_seq = 0;
    portEXIT_CRITICAL(&s_last_card_lock);
    s_active = false;
}

/** Generate a fresh six-character alphanumeric access code. */
static void admin_new_code(void)
{
    static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    for (size_t i = 0; i < ADMIN_ACCESS_CODE_LEN; i++)
    {
        s_code_str[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    s_code_str[ADMIN_ACCESS_CODE_LEN] = '\0';
    ESP_LOGI(TAG, "admin access code: %s", s_code_str);
    if (s_code_cb != NULL)
    {
        s_code_cb(s_code_str);
    }
}

esp_err_t admin_start(char *code_out, size_t code_size)
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
    if (code_out != NULL && code_size < ADMIN_ACCESS_CODE_LEN + 1)
    {
        return ESP_ERR_INVALID_SIZE;
    }


    /* File-management APIs require an operational SD mount. content_init() is
     * idempotent when application startup mounted it already. */
    err = content_init();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "content_init failed; starting admin without SD: %s",
                 esp_err_to_name(err));
    }
    /* Render while heap is still unconstrained by the Wi-Fi/HTTP stacks. */
    admin_new_code();
    admin_log_heap("before Wi-Fi");

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
    httpd_cfg.max_uri_handlers = 20;
    httpd_cfg.max_open_sockets = 4;
    httpd_cfg.stack_size = 16384;
    httpd_cfg.lru_purge_enable = true;
    httpd_cfg.uri_match_fn = httpd_uri_match_wildcard;
    admin_log_heap("before HTTP server");
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

    s_active = true;

    if (code_out != NULL)
    {
        memcpy(code_out, s_code_str, ADMIN_ACCESS_CODE_LEN + 1);
    }

    admin_log_heap("admin active");
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

void admin_set_last_card(const uint8_t *uid, uint8_t uid_len,
                         const char *url)
{
    const char *new_url = url == NULL ? "" : url;
    bool changed = false;

    portENTER_CRITICAL(&s_last_card_lock);
    if (uid == NULL || uid_len == 0 || uid_len > sizeof(s_last_card_uid))
    {
        changed = s_last_card_captured;
        s_last_card_uid_len = 0;
        s_last_card_url[0] = '\0';
        s_last_card_captured = false;
    }
    else
    {
        changed = !s_last_card_captured
               || s_last_card_uid_len != uid_len
               || memcmp(s_last_card_uid, uid, uid_len) != 0
               || strcmp(s_last_card_url, new_url) != 0;
        if (changed)
        {
            memcpy(s_last_card_uid, uid, uid_len);
            s_last_card_uid_len = uid_len;
            snprintf(s_last_card_url, sizeof(s_last_card_url), "%s", new_url);
            s_last_card_captured = true;
            s_last_card_seq++;
        }
    }
    portEXIT_CRITICAL(&s_last_card_lock);

    if (changed && uid != NULL && uid_len > 0)
    {
        char uid_hex[ADMIN_CARD_UID_MAX * 2 + 1];
        admin_format_uid(uid, uid_len, uid_hex, sizeof(uid_hex));
        ESP_LOGI(TAG, "captured card UID=%s URL=%s", uid_hex,
                 new_url[0] == '\0' ? "(blank)" : new_url);
    }
}

void admin_set_code_callback(admin_code_cb_t cb)
{
    s_code_cb = cb;
}

void admin_set_card_write_callback(admin_card_write_cb_t cb)
{
    s_write_card_cb = cb;
}

void admin_set_path_callbacks(admin_path_cb_t play_sound,
                              admin_path_cb_t display_image,
                              admin_action_cb_t stop_sound,
                              admin_action_cb_t clear_display)
{
    s_play_sound_cb = play_sound;
    s_display_image_cb = display_image;
    s_stop_sound_cb = stop_sound;
    s_clear_display_cb = clear_display;
}
