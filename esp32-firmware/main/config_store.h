#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#define CONFIG_STORE_NAMESPACE   "esp32cam"
#define CONFIG_STORE_KEY         "device_cfg"
#define CONFIG_STORE_MAGIC       0x43504D31u /* "CPM1" */
#define CONFIG_STORE_VERSION     2u
#define CONFIG_SSID_MAX_LEN      32
#define CONFIG_PASSWORD_MAX_LEN  64
#define CONFIG_HOST_MAX_LEN      63
#define CONFIG_TURN_URL_MAX_LEN  127
#define CONFIG_RESERVED_LEN      64

typedef struct {
    char     wifi_ssid[CONFIG_SSID_MAX_LEN + 1];      /* NUL-terminated */
    char     wifi_password[CONFIG_PASSWORD_MAX_LEN + 1];
    char     backend_host[CONFIG_HOST_MAX_LEN + 1];   /* signaling host */
    uint32_t backend_port;                            /* 1..65535 */
    char     turn_server[CONFIG_TURN_URL_MAX_LEN + 1];/* "turn:host:port", optional */
    bool     auto_flash;                              /* stored-only for now */
    uint8_t  reserved[CONFIG_RESERVED_LEN];           /* 0 on write */
} device_config_t;

/**
 * Load the stored config into *out.
 *
 * Returns:
 *  ESP_OK                 - valid config copied to *out
 *  ESP_ERR_NOT_FOUND      - blob absent (first flash / erased NVS)
 *  ESP_ERR_INVALID_STATE  - magic/version mismatch or field validation failed
 *  other                  - NVS errors
 */
esp_err_t config_store_load(device_config_t *out);

/**
 * Store *cfg (validated). Fills magic/version/reserved internally, writes
 * the blob and commits - a single blob write is atomic within NVS.
 */
esp_err_t config_store_save(const device_config_t *cfg);

/** Pure field validation, no NVS. ESP_OK or ESP_ERR_INVALID_ARG. */
esp_err_t config_validate(const device_config_t *cfg);

/** Zeroes cfg and applies safe defaults (port 8080, bools false, empty strings). */
void config_init_defaults(device_config_t *cfg);
