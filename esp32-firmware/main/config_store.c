#include "config_store.h"

#include <esp_log.h>
#include <nvs.h>
#include <string.h>

#define TAG "cfg_store"

/* magic + version travel with the blob so schema evolution can bump
 * CONFIG_STORE_VERSION without invalidating older devices' entries. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    device_config_t cfg;
} config_blob_t;

void config_init_defaults(device_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
}

esp_err_t config_validate(const device_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strlen(cfg->wifi_ssid);
    if (ssid_len == 0 || ssid_len > CONFIG_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < ssid_len; i++) {
        unsigned char c = (unsigned char)cfg->wifi_ssid[i];
        if (c < 0x20 || c > 0x7e) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (strlen(cfg->wifi_password) > CONFIG_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG; /* empty = open network, allowed */
    }

    size_t host_len = strlen(cfg->backend_host);
    if (host_len == 0 || host_len > CONFIG_HOST_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    /* A ws:// or http:// prefix marks the local-dev mode (plain WS to a
     * desktop `wrangler dev`); past the prefix the host may also carry a
     * port (":"), e.g. ws://192.168.1.50:8787. Production hostnames stay
     * scheme-less. */
    size_t start = 0;
    int with_port = 0;
    if (strncmp(cfg->backend_host, "ws://", 5) == 0 ||
        strncmp(cfg->backend_host, "http://", 7) == 0) {
        start = (size_t)(strstr(cfg->backend_host, "://") - cfg->backend_host) + 3;
        if (start >= host_len) {
            return ESP_ERR_INVALID_ARG; /* scheme prefix with no host */
        }
        with_port = 1;
    }
    for (size_t i = start; i < host_len; i++) {
        char c = cfg->backend_host[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' ||
              (with_port && c == ':'))) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* The token rides in an HTTP header, so it must be printable ASCII
     * without spaces/control chars. An empty token would make the Worker
     * reject every handshake - require one at config time. */
    size_t token_len = strlen(cfg->auth_token);
    if (token_len == 0 || token_len > CONFIG_TOKEN_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < token_len; i++) {
        unsigned char c = (unsigned char)cfg->auth_token[i];
        if (c < 0x21 || c > 0x7e) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

esp_err_t config_store_load(device_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    config_blob_t blob;
    size_t len = sizeof(blob);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_STORE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return ESP_ERR_NOT_FOUND; /* namespace never created: first flash */
        }
        return err;
    }

    err = nvs_get_blob(handle, CONFIG_STORE_KEY, &blob, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return ESP_ERR_NOT_FOUND;
        }
        return err;
    }

    if (len != sizeof(blob) || blob.magic != CONFIG_STORE_MAGIC ||
        blob.version != CONFIG_STORE_VERSION) {
        ESP_LOGW(TAG, "Stored config corrupt (len=%u magic=0x%lx version=%lu)",
                 (unsigned)len, (unsigned long)blob.magic, (unsigned long)blob.version);
        return ESP_ERR_INVALID_STATE;
    }

    if (config_validate(&blob.cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Stored config failed validation");
        return ESP_ERR_INVALID_STATE;
    }

    *out = blob.cfg;
    return ESP_OK;
}

esp_err_t config_store_save(const device_config_t *cfg)
{
    if (cfg == NULL || config_validate(cfg) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    config_blob_t blob = {
        .magic = CONFIG_STORE_MAGIC,
        .version = CONFIG_STORE_VERSION,
        .cfg = *cfg,
    };
    memset(blob.cfg.reserved, 0, sizeof(blob.cfg.reserved));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, CONFIG_STORE_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
