#include "ota.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>
#include <string.h>
#include <stdio.h>

#define TAG "ota"

// GitHub's release JSON includes the full release-notes body, which can be
// large; this only needs to be bigger than tag_name + the assets array.
#define OTA_BUFFER_SIZE 8192

typedef struct {
    char data[OTA_BUFFER_SIZE];
    size_t len;
} http_response_t;

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *resp = (http_response_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (resp->len + evt->data_len < OTA_BUFFER_SIZE) {
                memcpy(&resp->data[resp->len], evt->data, evt->data_len);
                resp->len += evt->data_len;
            } else {
                ESP_LOGW(TAG, "release response exceeds %d bytes, truncating (JSON parse will likely fail)",
                         OTA_BUFFER_SIZE);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t ota_check_latest_release(const char *owner, const char *repo, ota_release_info_t *info)
{
    if (!owner || !repo || !info) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/releases/latest", owner, repo);

    http_response_t response = {0};

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = _http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP status code: %d", status_code);
        return ESP_FAIL;
    }

    if (response.len == 0) {
        ESP_LOGE(TAG, "Empty response");
        return ESP_FAIL;
    }

    response.data[response.len] = '\0';

    cJSON *root = cJSON_Parse(response.data);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return ESP_FAIL;
    }

    cJSON *tag_name_item = cJSON_GetObjectItem(root, "tag_name");
    if (!tag_name_item || !tag_name_item->valuestring) {
        ESP_LOGE(TAG, "tag_name not found in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strncpy(info->tag_name, tag_name_item->valuestring, sizeof(info->tag_name) - 1);
    info->tag_name[sizeof(info->tag_name) - 1] = '\0';

    cJSON *assets = cJSON_GetObjectItem(root, "assets");
    if (!assets || !cJSON_IsArray(assets)) {
        ESP_LOGE(TAG, "assets not found in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    info->update_available = false;
    for (int i = 0; i < cJSON_GetArraySize(assets); i++) {
        cJSON *asset = cJSON_GetArrayItem(assets, i);
        cJSON *name_item = cJSON_GetObjectItem(asset, "name");
        cJSON *url_item = cJSON_GetObjectItem(asset, "browser_download_url");

        if (name_item && url_item && name_item->valuestring && url_item->valuestring) {
            if (strstr(name_item->valuestring, ".bin")) {
                strncpy(info->url, url_item->valuestring, sizeof(info->url) - 1);
                info->url[sizeof(info->url) - 1] = '\0';
                info->update_available = true;
                ESP_LOGI(TAG, "Found binary asset: %s", name_item->valuestring);
                break;
            }
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ota_apply(const char *firmware_url)
{
    if (!firmware_url) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting OTA update from: %s", firmware_url);

    esp_https_ota_config_t ota_config = {
        .http_config = &(esp_http_client_config_t){
            .url = firmware_url,
            .crt_bundle_attach = esp_crt_bundle_attach,
        },
    };

    esp_err_t err = esp_https_ota(&ota_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA update completed successfully");
    return ESP_OK;
}
