/*
 * content.c — SD-card content store: mapping.json index + media directory.
 *
 * Mounts a FatFS-formatted SD card on the SDMMC peripheral at /sdcard (1-bit
 * bus on rev #05, 4-bit bus on rev #04 — see board_pins.h), keeps a
 * mapping.json index in memory as a cJSON tree, and persists
 * every mutation back to the card. Lookups return the relative media paths
 * recorded in the mapping ("media/<file>"); callers join them onto
 * CONTENT_MOUNT_POINT to open the real file.
 */
#include "content.h"

#include "board_pins.h"

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

static const char *TAG = "content";

#define CONTENT_MAP_PATH    CONTENT_MOUNT_POINT "/mapping.json"
#define CONTENT_MEDIA_DIR   CONTENT_MOUNT_POINT "/media"
#define CONTENT_MEDIA_PREFIX "media"

/* Upper bound for a persisted media path ("media/<nested/name>"). */
#define CONTENT_MEDIA_PATH_MAX 128

/* Values recovered from the stock sd_mount_card path. */
#define CONTENT_MAX_OPEN_FILES 10
#define CONTENT_MOUNT_ATTEMPTS 5
#define CONTENT_MOUNT_RETRY_MS 200

/* Handle returned by the mount; retained for later deinit/format. */
static sdmmc_card_t *s_card;

/* Parsed mapping.json: s_root = { "cards": [...] }, s_cards = the array. */
static cJSON *s_root;
static cJSON *s_cards;

static bool s_mounted;
static bool s_index_loaded;
static SemaphoreHandle_t s_mutex;

static bool content_lock(void)
{
    return s_mutex != NULL && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE;
}

static void content_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

/*
 * Catalog operations call these helpers only while content_lock() is held.
 * Keeping the cJSON tree locked through content_save() prevents readers from
 * traversing it while a mutation is being persisted.
 */

/*
 * Normalize a caller-provided media path into the persisted "media/..." form.
 * Legacy single-file callers may provide a bare filename; playlist callers may
 * include safe nested paths below media/. The catalog never records paths that
 * escape the media directory.
 */
static bool content_make_media_path(const char *name, char *out, size_t cap)
{
    const char *relative;
    const char *p;
    int written;

    if (name == NULL || out == NULL || cap == 0)
    {
        return false;
    }
    if (strncmp(name, CONTENT_MEDIA_PREFIX "/", strlen(CONTENT_MEDIA_PREFIX) + 1) == 0)
    {
        relative = name + strlen(CONTENT_MEDIA_PREFIX) + 1;
    }
    else
    {
        if (strchr(name, '/') != NULL)
        {
            return false;
        }
        relative = name;
    }
    if (*relative == '\0')
    {
        return false;
    }

    p = relative;
    while (*p != '\0')
    {
        const char *segment = p;
        size_t len;

        while (*p == '/')
        {
            p++;
        }
        segment = p;
        while (*p != '\0' && *p != '/')
        {
            unsigned char ch = (unsigned char)*p;

            if (ch < 0x20 || ch == '\\')
            {
                return false;
            }
            p++;
        }
        len = (size_t)(p - segment);
        if (len == 0 || (len == 1 && segment[0] == '.')
            || (len == 2 && segment[0] == '.' && segment[1] == '.'))
        {
            return false;
        }
    }

    written = snprintf(out, cap, "%s/%s", CONTENT_MEDIA_PREFIX, relative);
    return written > 0 && (size_t)written < cap;
}

/*
 * Serialize the in-memory cJSON tree back to /sdcard/mapping.json.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM on allocation failure,
 *         ESP_FAIL on any file IO error.
 */
static esp_err_t content_save(void)
{
    FILE *f;
    char *json;
    size_t len;
    size_t written;
    int rc;
    bool backed_up = false;

    if (s_root == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    json = cJSON_Print(s_root);
    if (json == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    f = fopen(CONTENT_MAP_PATH ".tmp", "w");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "open mapping temp failed: %s", strerror(errno));
        cJSON_free(json);
        return ESP_FAIL;
    }

    len = strlen(json);
    written = fwrite(json, 1, len, f);
    rc = fclose(f);
    cJSON_free(json);

    if (written != len || rc != 0)
    {
        ESP_LOGE(TAG, "write mapping temp failed: %u/%u close=%d errno=%s",
                 (unsigned)written, (unsigned)len, rc, strerror(errno));
        unlink(CONTENT_MAP_PATH ".tmp");
        return ESP_FAIL;
    }

    /*
     * FatFS may not overwrite an existing destination during rename(). Move
     * the old index aside, install the new one, and restore the old file on
     * failure. This makes delete/wipe/add persistence transactional.
     */
    unlink(CONTENT_MAP_PATH ".bak");
    if (rename(CONTENT_MAP_PATH, CONTENT_MAP_PATH ".bak") == 0)
    {
        backed_up = true;
    }
    else if (errno != ENOENT)
    {
        ESP_LOGE(TAG, "backup mapping failed: %s", strerror(errno));
        unlink(CONTENT_MAP_PATH ".tmp");
        return ESP_FAIL;
    }

    if (rename(CONTENT_MAP_PATH ".tmp", CONTENT_MAP_PATH) != 0)
    {
        ESP_LOGE(TAG, "install mapping failed: %s", strerror(errno));
        unlink(CONTENT_MAP_PATH ".tmp");
        if (backed_up && rename(CONTENT_MAP_PATH ".bak", CONTENT_MAP_PATH) != 0)
        {
            ESP_LOGE(TAG, "restore mapping backup failed: %s", strerror(errno));
        }
        return ESP_FAIL;
    }
    if (backed_up && unlink(CONTENT_MAP_PATH ".bak") != 0)
    {
        ESP_LOGW(TAG, "remove mapping backup failed: %s", strerror(errno));
    }
    return ESP_OK;
}

/*
 * Read and parse /sdcard/mapping.json into the s_root/s_cards tree. Frees any
 * previously loaded tree first.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM on allocation failure,
 *         ESP_FAIL on IO or parse error.
 */
static esp_err_t content_load(void)
{
    FILE *f;
    long size;
    char *buf;
    size_t n;
    cJSON *root;
    cJSON *cards;

    f = fopen(CONTENT_MAP_PATH, "rb");
    if (f == NULL)
    {
        return ESP_FAIL;
    }

    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return ESP_FAIL;
    }
    size = ftell(f);
    if (size < 0)
    {
        fclose(f);
        return ESP_FAIL;
    }
    if (fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return ESP_FAIL;
    }

    buf = malloc((size_t)size + 1);
    if (buf == NULL)
    {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';

    root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL)
    {
        return ESP_FAIL;
    }

    cards = cJSON_GetObjectItemCaseSensitive(root, "cards");
    if (cards == NULL || !cJSON_IsArray(cards))
    {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    if (s_root != NULL)
    {
        cJSON_Delete(s_root);
    }

    s_root = root;
    s_cards = cards;
    return ESP_OK;
}

static esp_err_t content_create_empty_index(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *cards;

    if (root == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    cards = cJSON_CreateArray();
    if (cards == NULL)
    {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "cards", cards);

    if (s_root != NULL)
    {
        cJSON_Delete(s_root);
    }
    s_root = root;
    s_cards = cards;
    return ESP_OK;
}

static esp_err_t content_ensure_index_loaded(void)
{
    struct stat st;
    esp_err_t err;

    if (s_index_loaded)
    {
        return ESP_OK;
    }
    if (!s_mounted)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (stat(CONTENT_MAP_PATH, &st) == 0)
    {
        ESP_LOGI(TAG, "loading optional index %s on demand", CONTENT_MAP_PATH);
        err = content_load();
        if (err == ESP_ERR_NO_MEM)
        {
            return err;
        }
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "ignoring invalid optional index %s: %s",
                     CONTENT_MAP_PATH, esp_err_to_name(err));
            err = content_create_empty_index();
        }
    }
    else
    {
        err = content_create_empty_index();
    }
    if (err == ESP_OK)
    {
        s_index_loaded = true;
    }
    return err;
}


esp_err_t content_init(void)
{
    esp_err_t err;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mount_config;
    int attempt;

    if (s_mounted)
    {
        return ESP_OK;
    }
    if (s_mutex == NULL)
    {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL)
        {
            ESP_LOGE(TAG, "could not create content mutex");
            return ESP_ERR_NO_MEM;
        }
    }


    /* Stock firmware drives SD_CLK at the ESP32's maximum 40 mA and clocks
     * SDMMC at 40 MHz. The stronger clock edge is required on the Yoto PCB. */
    err = gpio_set_drive_capability(PIN_SD_CLK, GPIO_DRIVE_CAP_3);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "could not set SD clock drive strength: %s",
                 esp_err_to_name(err));
    }
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
#ifdef CONFIG_BOARD_REV_04
    host.flags = SDMMC_HOST_FLAG_4BIT;
#else
    host.flags = SDMMC_HOST_FLAG_1BIT;
#endif

    /* 1-bit bus on the Yoto's SDMMC pins (see board_pins.h). */
    slot_config.clk = PIN_SD_CLK;
    slot_config.cmd = PIN_SD_CMD;
    slot_config.d0 = PIN_SD_D0;
#ifdef CONFIG_BOARD_REV_04
    /* Rev #04 (hwconfig_04 "sd4"): 4-bit bus; d1/d2/d3 from board_pins.h. */
    slot_config.d1 = PIN_SD_D1;
    slot_config.d2 = PIN_SD_D2;
    slot_config.d3 = PIN_SD_D3;
    slot_config.width = BOARD_SD_WIDTH;
#else
    slot_config.width = BOARD_SD_WIDTH;
#endif

    mount_config.format_if_mount_failed = false;
    mount_config.max_files = CONTENT_MAX_OPEN_FILES;
    mount_config.allocation_unit_size = 16 * 1024;
    mount_config.disk_status_check_enable = false;
    mount_config.use_one_fat = false;

    err = ESP_FAIL;
    for (attempt = 1; attempt <= CONTENT_MOUNT_ATTEMPTS; attempt++)
    {
        ESP_LOGI(TAG, "mounting SD card (attempt %d/%d)",
                 attempt, CONTENT_MOUNT_ATTEMPTS);
        err = esp_vfs_fat_sdmmc_mount(CONTENT_MOUNT_POINT, &host,
                                      &slot_config, &mount_config, &s_card);
        if (err == ESP_OK)
        {
            break;
        }
        if (attempt < CONTENT_MOUNT_ATTEMPTS)
        {
            vTaskDelay(pdMS_TO_TICKS(CONTENT_MOUNT_RETRY_MS));
        }
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SD card mount failed at %s after %d attempts: %s",
                 CONTENT_MOUNT_POINT, CONTENT_MOUNT_ATTEMPTS,
                 esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s", CONTENT_MOUNT_POINT);
    if (mkdir(CONTENT_MEDIA_DIR, 0755) != 0 && errno != EEXIST)
    {
        ESP_LOGW(TAG, "could not ensure media directory %s: errno=%d (%s)",
                 CONTENT_MEDIA_DIR, errno, strerror(errno));
    }

    /* mapping.json is not needed to mount SD or start the web UI. Parse it
     * only when NFC mapping or the Cards tab first requests it. */
    s_index_loaded = false;
    ESP_LOGI(TAG, "content index deferred until first use");
    ESP_LOGI(TAG, "content store ready at %s", CONTENT_MOUNT_POINT);
    return ESP_OK;
}

static esp_err_t content_lookup_locked(const char *url, char *sound_path,
                                       size_t sp, char *image_path, size_t ip)
{
    cJSON *card;
    cJSON *u;
    cJSON *sound;
    cJSON *image;

    esp_err_t index_err = content_ensure_index_loaded();
    if (index_err != ESP_OK)
    {
        return index_err;
    }

    if (s_root == NULL || s_cards == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (url == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (sound_path != NULL && sp > 0)
    {
        sound_path[0] = '\0';
    }
    if (image_path != NULL && ip > 0)
    {
        image_path[0] = '\0';
    }

    cJSON_ArrayForEach(card, s_cards)
    {
        u = cJSON_GetObjectItemCaseSensitive(card, "url");
        if (u == NULL || !cJSON_IsString(u) || u->valuestring == NULL)
        {
            continue;
        }
        if (strcmp(u->valuestring, url) != 0)
        {
            continue;
        }

        sound = cJSON_GetObjectItemCaseSensitive(card, "sound");
        if (sound != NULL && cJSON_IsString(sound) && sound->valuestring != NULL
            && sound_path != NULL && sp > 0)
        {
            snprintf(sound_path, sp, "%s", sound->valuestring);
        }

        image = cJSON_GetObjectItemCaseSensitive(card, "image");
        if (image != NULL && cJSON_IsString(image) && image->valuestring != NULL
            && image_path != NULL && ip > 0)
        {
            snprintf(image_path, ip, "%s", image->valuestring);
        }

        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t content_lookup(const char *url, char *sound_path, size_t sp,
                         char *image_path, size_t ip)
{
    esp_err_t err;

    if (!content_lock())
    {
        return ESP_ERR_INVALID_STATE;
    }
    err = content_lookup_locked(url, sound_path, sp, image_path, ip);
    content_unlock();
    return err;
}

/*
 * Find the mapping card whose "url" matches url, or NULL if absent.
 *
 * @param url URL key to match.
 * @return the cJSON card object, or NULL.
 */
static cJSON *content_find_card(const char *url)
{
    cJSON *card;
    cJSON *u;

    if (content_ensure_index_loaded() != ESP_OK)
    {
        return NULL;
    }

    if (s_root == NULL || s_cards == NULL || url == NULL)
    {
        return NULL;
    }

    cJSON_ArrayForEach(card, s_cards)
    {
        u = cJSON_GetObjectItemCaseSensitive(card, "url");
        if (u != NULL && cJSON_IsString(u) && u->valuestring != NULL
            && strcmp(u->valuestring, url) == 0)
        {
            return card;
        }
    }
    return NULL;
}

static int content_get_track_count_locked(const char *url)
{
    cJSON *card;
    cJSON *tracks;
    cJSON *sound;

    card = content_find_card(url);
    if (card == NULL)
    {
        return -1;
    }

    tracks = cJSON_GetObjectItemCaseSensitive(card, "tracks");
    if (tracks != NULL && cJSON_IsArray(tracks))
    {
        return cJSON_GetArraySize(tracks);
    }

    sound = cJSON_GetObjectItemCaseSensitive(card, "sound");
    if (sound != NULL && cJSON_IsString(sound) && sound->valuestring != NULL)
    {
        return 1;
    }

    return 0;
}

int content_get_track_count(const char *url)
{
    int count;

    if (!content_lock())
    {
        return -1;
    }
    count = content_get_track_count_locked(url);
    content_unlock();
    return count;
}

static esp_err_t content_get_track_locked(const char *url, int index,
                                          char *sound_path, size_t sp)
{
    cJSON *card;
    cJSON *tracks;
    const char *val;

    card = content_find_card(url);
    if (card == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    val = NULL;
    tracks = cJSON_GetObjectItemCaseSensitive(card, "tracks");
    if (tracks != NULL && cJSON_IsArray(tracks))
    {
        cJSON *item = cJSON_GetArrayItem(tracks, index);
        if (item != NULL && cJSON_IsString(item))
        {
            val = item->valuestring;
        }
    }
    else if (index == 0)
    {
        cJSON *sound = cJSON_GetObjectItemCaseSensitive(card, "sound");
        if (sound != NULL && cJSON_IsString(sound))
        {
            val = sound->valuestring;
        }
    }

    if (val == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (sound_path != NULL && sp > 0)
    {
        if (strncmp(val, CONTENT_MOUNT_POINT "/",
                    strlen(CONTENT_MOUNT_POINT) + 1) == 0)
        {
            snprintf(sound_path, sp, "%s", val);
        }
        else
        {
            snprintf(sound_path, sp, CONTENT_MOUNT_POINT "/%s", val);
        }
    }
    return ESP_OK;
}

esp_err_t content_get_track(const char *url, int index,
                            char *sound_path, size_t sp)
{
    esp_err_t err;

    if (!content_lock())
    {
        return ESP_ERR_INVALID_STATE;
    }
    err = content_get_track_locked(url, index, sound_path, sp);
    content_unlock();
    return err;
}

static esp_err_t content_add_locked(const char *url, const char *sound_name,
                                    const char *image_name)
{
    cJSON *card;
    cJSON *new_card;
    char sound_media[CONTENT_MEDIA_PATH_MAX];
    char image_media[CONTENT_MEDIA_PATH_MAX];
    int idx;
    int i;

    esp_err_t index_err = content_ensure_index_loaded();
    if (index_err != ESP_OK)
    {
        return index_err;
    }
    if (url == NULL || sound_name == NULL || image_name == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!content_make_media_path(sound_name, sound_media, sizeof(sound_media))
        || (image_name[0] != '\0'
            && !content_make_media_path(image_name, image_media,
                                        sizeof(image_media))))
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* Locate any existing card with the same URL so adds stay idempotent. */
    idx = -1;
    i = 0;
    cJSON_ArrayForEach(card, s_cards)
    {
        cJSON *u = cJSON_GetObjectItemCaseSensitive(card, "url");
        if (u != NULL && cJSON_IsString(u) && u->valuestring != NULL
            && strcmp(u->valuestring, url) == 0)
        {
            idx = i;
            break;
        }
        i++;
    }
    if (idx >= 0)
    {
        cJSON_DeleteItemFromArray(s_cards, idx);
    }

    new_card = cJSON_CreateObject();
    if (new_card == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    /* Paths were normalized and validated before replacing the old mapping. */

    cJSON_AddStringToObject(new_card, "url", url);
    cJSON_AddStringToObject(new_card, "sound", sound_media);
    if (image_name[0] != '\0')
    {
        cJSON_AddStringToObject(new_card, "image", image_media);
    }

    cJSON_AddItemToArray(s_cards, new_card);

    return content_save();
}

esp_err_t content_add(const char *url, const char *sound_name,
                      const char *image_name)
{
    esp_err_t err;

    if (!content_lock())
    {
        return ESP_ERR_INVALID_STATE;
    }
    err = content_add_locked(url, sound_name, image_name);
    content_unlock();
    return err;
}

static esp_err_t content_delete_locked(const char *url)
{
    cJSON *card;
    cJSON *detached;
    int idx;
    int i;

    esp_err_t index_err = content_ensure_index_loaded();
    if (index_err != ESP_OK)
    {
        return index_err;
    }
    if (url == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    idx = -1;
    i = 0;
    cJSON_ArrayForEach(card, s_cards)
    {
        cJSON *u = cJSON_GetObjectItemCaseSensitive(card, "url");
        if (u != NULL && cJSON_IsString(u) && u->valuestring != NULL
            && strcmp(u->valuestring, url) == 0)
        {
            idx = i;
            break;
        }
        i++;
    }

    if (idx < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    detached = cJSON_DetachItemFromArray(s_cards, idx);
    if (detached == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    index_err = content_save();
    if (index_err != ESP_OK)
    {
        /* Restore the exact position when mapping.json could not be replaced. */
        cJSON_InsertItemInArray(s_cards, idx, detached);
        return index_err;
    }
    cJSON_Delete(detached);
    return ESP_OK;
}

esp_err_t content_delete(const char *url)
{
    esp_err_t err;

    if (!content_lock())
    {
        return ESP_ERR_INVALID_STATE;
    }
    err = content_delete_locked(url);
    content_unlock();
    return err;
}

static esp_err_t content_delete_all_locked(void)
{
    cJSON *old_cards;
    cJSON *empty_cards;
    cJSON *discarded_cards;
    esp_err_t err;

    err = content_ensure_index_loaded();
    if (err != ESP_OK)
    {
        return err;
    }

    empty_cards = cJSON_CreateArray();
    if (empty_cards == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    old_cards = cJSON_DetachItemFromObjectCaseSensitive(s_root, "cards");
    if (old_cards == NULL)
    {
        cJSON_Delete(empty_cards);
        return ESP_ERR_INVALID_STATE;
    }
    cJSON_AddItemToObjectCS(s_root, "cards", empty_cards);
    s_cards = empty_cards;

    err = content_save();
    if (err != ESP_OK)
    {
        discarded_cards = cJSON_DetachItemFromObjectCaseSensitive(s_root,
                                                                   "cards");
        cJSON_AddItemToObjectCS(s_root, "cards", old_cards);
        s_cards = old_cards;
        cJSON_Delete(discarded_cards);
        return err;
    }

    cJSON_Delete(old_cards);
    return ESP_OK;
}

esp_err_t content_delete_all(void)
{
    esp_err_t err;

    if (!content_lock())
    {
        return ESP_ERR_INVALID_STATE;
    }
    err = content_delete_all_locked();
    content_unlock();
    return err;
}

static esp_err_t content_list_json_locked(char **out)
{
    esp_err_t err;

    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    err = content_ensure_index_loaded();
    if (err != ESP_OK)
    {
        return err;
    }
    *out = cJSON_PrintUnformatted(s_cards);
    return *out == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t content_list_json(char **out)
{
    esp_err_t err;

    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!content_lock())
    {
        return ESP_ERR_INVALID_STATE;
    }
    err = content_list_json_locked(out);
    content_unlock();
    return err;
}

static esp_err_t content_add_playlist_locked(
    const char *url,
    const char *name,
    const char *const tracks[],
    const char *const track_images[],
    int n,
    const char *cover_image)
{
    cJSON *card;
    cJSON *new_card;
    cJSON *tracks_arr;
    cJSON *images_arr;
    const char *display_name;
    char media[CONTENT_MEDIA_PATH_MAX];
    int idx;
    int i;

    esp_err_t index_err = content_ensure_index_loaded();
    if (index_err != ESP_OK)
    {
        return index_err;
    }
    if (url == NULL || tracks == NULL || n <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    display_name = (name != NULL && name[0] != '\0') ? name : url;

    for (i = 0; i < n; i++)
    {
        if (tracks[i] == NULL
            || !content_make_media_path(tracks[i], media, sizeof(media))
            || (track_images != NULL && track_images[i] != NULL
                && track_images[i][0] != '\0'
                && !content_make_media_path(track_images[i], media, sizeof(media))))
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (cover_image != NULL && cover_image[0] != '\0'
        && !content_make_media_path(cover_image, media, sizeof(media)))
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* Locate and drop any existing card with the same URL (idempotent). */
    idx = -1;
    i = 0;
    cJSON_ArrayForEach(card, s_cards)
    {
        cJSON *u = cJSON_GetObjectItemCaseSensitive(card, "url");
        if (u != NULL && cJSON_IsString(u) && u->valuestring != NULL
            && strcmp(u->valuestring, url) == 0)
        {
            idx = i;
            break;
        }
        i++;
    }
    if (idx >= 0)
    {
        cJSON_DeleteItemFromArray(s_cards, idx);
    }

    new_card = cJSON_CreateObject();
    tracks_arr = cJSON_CreateArray();
    images_arr = cJSON_CreateArray();
    if (new_card == NULL || tracks_arr == NULL || images_arr == NULL)
    {
        cJSON_Delete(new_card);
        cJSON_Delete(tracks_arr);
        cJSON_Delete(images_arr);
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < n; i++)
    {
        content_make_media_path(tracks[i], media, sizeof(media));
        cJSON_AddItemToArray(tracks_arr, cJSON_CreateString(media));

        if (track_images != NULL && track_images[i] != NULL
            && track_images[i][0] != '\0')
        {
            content_make_media_path(track_images[i], media, sizeof(media));
            cJSON_AddItemToArray(images_arr, cJSON_CreateString(media));
        }
        else
        {
            cJSON_AddItemToArray(images_arr, cJSON_CreateString(""));
        }
    }

    cJSON_AddStringToObject(new_card, "url", url);
    cJSON_AddStringToObject(new_card, "name", display_name);
    cJSON_AddItemToObject(new_card, "tracks", tracks_arr);
    cJSON_AddItemToObject(new_card, "track_images", images_arr);

    if (cover_image != NULL && cover_image[0] != '\0')
    {
        content_make_media_path(cover_image, media, sizeof(media));
        cJSON_AddStringToObject(new_card, "image", media);
    }

    cJSON_AddItemToArray(s_cards, new_card);

    return content_save();
}

esp_err_t content_add_playlist(const char *url,
                               const char *name,
                               const char *const tracks[],
                               const char *const track_images[],
                               int n,
                               const char *cover_image)
{
    esp_err_t err;

    if (!content_lock())
    {
        return ESP_ERR_INVALID_STATE;
    }
    err = content_add_playlist_locked(url, name, tracks, track_images, n,
                                      cover_image);
    content_unlock();
    return err;
}

static esp_err_t content_get_track_image_locked(const char *url, int index,
                                                char *image_path, size_t ip)
{
    cJSON *card;
    cJSON *images;
    cJSON *item;
    cJSON *cover;
    const char *val = NULL;

    if (image_path != NULL && ip > 0)
    {
        image_path[0] = '\0';
    }

    card = content_find_card(url);
    if (card == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    images = cJSON_GetObjectItemCaseSensitive(card, "track_images");
    if (images != NULL && cJSON_IsArray(images))
    {
        item = cJSON_GetArrayItem(images, index);
        if (item != NULL && cJSON_IsString(item) && item->valuestring != NULL
            && item->valuestring[0] != '\0')
        {
            val = item->valuestring;
        }
    }

    if (val == NULL)
    {
        cover = cJSON_GetObjectItemCaseSensitive(card, "image");
        if (cover != NULL && cJSON_IsString(cover) && cover->valuestring != NULL
            && cover->valuestring[0] != '\0')
        {
            val = cover->valuestring;
        }
    }

    if (val == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (image_path != NULL && ip > 0)
    {
        snprintf(image_path, ip, "%s", val);
    }
    return ESP_OK;
}

esp_err_t content_get_track_image(const char *url, int index,
                                  char *image_path, size_t ip)
{
    esp_err_t err;

    if (!content_lock())
    {
        return ESP_ERR_INVALID_STATE;
    }
    err = content_get_track_image_locked(url, index, image_path, ip);
    content_unlock();
    return err;
}
