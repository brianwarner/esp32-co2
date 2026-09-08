/* SPDX-License-Identifier: Apache-2.0 */

#include "co2_history_store.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "co2hist";

#define NVS_NAMESPACE "co2hist"
#define NVS_KEY "hist"

/* Magic changes if the blob layout ever changes, so a build with a
 * different CO2_HISTORY_LEN (or field layout) doesn't misread a stale save
 * left over from an older firmware version. */
#define HISTORY_MAGIC 0x43483032u /* "CH02" */

typedef struct
{
    uint32_t magic;
    int64_t bucket_epoch;
    uint16_t history[CO2_HISTORY_LEN];
} history_blob_t;

bool co2_history_load(uint16_t history[CO2_HISTORY_LEN], long *bucket_epoch)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
    {
        return false;
    }

    history_blob_t blob;
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &blob, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(blob) || blob.magic != HISTORY_MAGIC)
    {
        return false;
    }

    memcpy(history, blob.history, sizeof(blob.history));
    *bucket_epoch = (long)blob.bucket_epoch;
    return true;
}

void co2_history_save(const uint16_t history[CO2_HISTORY_LEN], long bucket_epoch)
{
    history_blob_t blob = {
        .magic = HISTORY_MAGIC,
        .bucket_epoch = bucket_epoch,
    };
    memcpy(blob.history, history, sizeof(blob.history));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(h, NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "failed to save CO2 history: %s", esp_err_to_name(err));
    }
    nvs_close(h);
}
