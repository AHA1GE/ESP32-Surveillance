#pragma once

#include <esp_err.h>
#include <stddef.h>

/* MAC-derived identifiers: the device ID and the config AP SSID share the
 * same 6-hex-digit suffix (last 3 bytes of the WiFi STA MAC), so the AP you
 * join and the backend URL for that device visibly correspond. */

/** "esp32cam-XXXXXX" (lowercase hex). out must be >= 16 bytes. */
esp_err_t device_id_get(char *out, size_t out_len);

/** "ESP32-CAM-XXXXXX" (uppercase hex). out must be >= 17 bytes. */
esp_err_t device_id_get_ap_ssid(char *out, size_t out_len);
