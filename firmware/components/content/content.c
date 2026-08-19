/*
 * content.c — SD-card content store: mapping.json index + media directory.
 *
 * Mounts a FatFS-formatted SD card on the SDMMC peripheral in 1-bit mode at
 * /sdcard, keeps a mapping.json index in memory as a cJSON tree, and persists
 * every mutation back to the card. Lookups return the relative media paths
 * recorded in the mapping ("media/<file>"); callers join them onto
 * CONTENT_MOUNT_POINT to open the real file.
 */
#include "content.h"

#include "board_pins.h"

#include "cJSON.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

static const char *TAG = "content";

#define CONTENT_MAP_PATH    CONTENT_MOUNT_POINT "/mapping.json"
#define CONTENT_MEDIA_DIR   CONTENT_MOUNT_POINT "/media"
#define CONTENT_MEDIA_PREFIX "media"

/* Upper bound for a persisted media path ("media/<name>") built on add. */
#define CONTENT_MEDIA_PATH_MAX 128

/* Maximum open file descriptors FatFS should reserve for this component. */
#define CONTENT_MAX_OPEN_FILES 4

/* Handle returned by the mount; retained for later deinit/format. */
static sdmmc_card_t *s_card;

/* Parsed mapping.json: s_root = { "cards": [...] }, s_cards = the array. */
static cJSON *s_root;
static cJSON *s_cards;

static bool s_mounted;

/*
 * Build the persisted media path for a bare file name. A name that already
 * starts with "media" is kept verbatim; otherwise "media/" is prepended.
 *
 * @param name  bare file name (no directory).
 * @param out   output buffer.
 * @param cap   output buffer capacity in bytes.
 */
static void content_make_media_path(const char *name, char *out, size_t cap)
{
    size_t prefix_len;

    if (out == NULL || cap == 0)
    {
        return;
    }
    if (name == NULL)
    {
        out[0] = '\0';
        return;
    }

    prefix_len = strlen(CONTENT_MEDIA_PREFIX);
    if (strncmp(name, CONTENT_MEDIA_PREFIX, prefix_len) == 0)
    {
        snprintf(out, cap, "%s", name);
    }
    else
    {
        snprintf(out, cap, "%s/%s", CONTENT_MEDIA_PREFIX, name);
    }
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

    if (s_root == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    json = cJSON_Print(s_root);
    if (json == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    f = fopen(CONTENT_MAP_PATH, "w");
    if (f == NULL)
    {
        cJSON_free(json);
        return ESP_FAIL;
    }

    len = strlen(json);
    written = fwrite(json, 1, len, f);
    rc = fclose(f);
    cJSON_free(json);

    if (written != len || rc != 0)
    {
        return ESP_FAIL;
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

esp_err_t content_init(void)
{
    esp_err_t err;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mount_config;
    struct stat st;

    if (s_mounted)
    {
        return ESP_OK;
    }

    /* 1-bit bus on the Yoto's SDMMC pins (see board_pins.h). */
    slot_config.clk = PIN_SD_CLK;
    slot_config.cmd = PIN_SD_CMD;
    slot_config.d0 = PIN_SD_D0;
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    mount_config.format_if_mount_failed = false;
    mount_config.max_files = CONTENT_MAX_OPEN_FILES;
    mount_config.allocation_unit_size = 16 * 1024;
    mount_config.disk_status_check_enable = false;
    mount_config.use_one_fat = false;

    err = esp_vfs_fat_sdmmc_mount(CONTENT_MOUNT_POINT, &host, &slot_config,
                                  &mount_config, &s_card);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SD card mount failed at %s: %s",
                 CONTENT_MOUNT_POINT, esp_err_to_name(err));
        return err;
    }
    s_mounted = true;

    /* Ensure the media directory exists. */
    if (stat(CONTENT_MEDIA_DIR, &st) != 0)
    {
        if (mkdir(CONTENT_MEDIA_DIR, 0755) != 0)
        {
            ESP_LOGW(TAG, "could not create %s", CONTENT_MEDIA_DIR);
        }
    }

    /* Ensure mapping.json exists, then load it into memory. */
    if (stat(CONTENT_MAP_PATH, &st) != 0)
    {
        s_root = cJSON_CreateObject();
        if (s_root == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
        s_cards = cJSON_CreateArray();
        if (s_cards == NULL)
        {
            cJSON_Delete(s_root);
            s_root = NULL;
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(s_root, "cards", s_cards);

        err = content_save();
        if (err != ESP_OK)
        {
            return err;
        }
    }
    else
    {
        err = content_load();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "failed to load %s: %s",
                     CONTENT_MAP_PATH, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "content store ready at %s", CONTENT_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t content_lookup(const char *url, char *sound_path, size_t sp,
                         char *image_path, size_t ip)
{
    cJSON *card;
    cJSON *u;
    cJSON *sound;
    cJSON *image;

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

int content_get_track_count(const char *url)
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

esp_err_t content_get_track(const char *url, int index,
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
        snprintf(sound_path, sp, "%s", val);
    }
    return ESP_OK;
}

esp_err_t content_add(const char *url, const char *sound_name,
                      const char *image_name)
{
    cJSON *card;
    cJSON *new_card;
    char sound_media[CONTENT_MEDIA_PATH_MAX];
    char image_media[CONTENT_MEDIA_PATH_MAX];
    int idx;
    int i;

    if (s_root == NULL || s_cards == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (url == NULL || sound_name == NULL || image_name == NULL)
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

    content_make_media_path(sound_name, sound_media, sizeof(sound_media));
    content_make_media_path(image_name, image_media, sizeof(image_media));

    cJSON_AddStringToObject(new_card, "url", url);
    cJSON_AddStringToObject(new_card, "sound", sound_media);
    cJSON_AddStringToObject(new_card, "image", image_media);

    cJSON_AddItemToArray(s_cards, new_card);

    return content_save();
}

esp_err_t content_delete(const char *url)
{
    cJSON *card;
    int idx;
    int i;

    if (s_root == NULL || s_cards == NULL)
    {
        return ESP_ERR_INVALID_STATE;
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

    cJSON_DeleteItemFromArray(s_cards, idx);

    return content_save();
}

esp_err_t content_list(char *out, size_t cap)
{
    char *json;
    size_t len;

    if (out == NULL || cap == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cards == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    json = cJSON_PrintUnformatted(s_cards);
    if (json == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    len = strlen(json);
    if (len + 1 > cap)
    {
        cJSON_free(json);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out, json, len + 1);
    cJSON_free(json);
    return ESP_OK;
}
