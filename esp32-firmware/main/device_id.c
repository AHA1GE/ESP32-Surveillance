#include "device_id.h"

#include <esp_mac.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define TAG "device_id"

static bool s_have_mac = false;
static uint8_t s_mac[6];

static void mac_cache(void)
{
    if (s_have_mac) {
        return;
    }
    if (esp_read_mac(s_mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        s_have_mac = true;
    }
}

static void mac_suffix(char *out, size_t out_len)
{
    mac_cache();
    if (!s_have_mac) {
        /* fallback so the ID is never empty */
        snprintf(out, out_len, "unknown");
        return;
    }
    snprintf(out, out_len, "%02x%02x%02x", s_mac[3], s_mac[4], s_mac[5]);
}

esp_err_t device_id_get(char *out, size_t out_len)
{
    if (out == NULL || out_len < 16) {
        return ESP_ERR_INVALID_ARG;
    }
    char suffix[7];
    mac_suffix(suffix, sizeof(suffix));
    snprintf(out, out_len, "esp32cam-%s", suffix);
    return ESP_OK;
}

esp_err_t device_id_get_ap_ssid(char *out, size_t out_len)
{
    if (out == NULL || out_len < 17) {
        return ESP_ERR_INVALID_ARG;
    }
    char suffix[7];
    mac_suffix(suffix, sizeof(suffix));
    snprintf(out, out_len, "ESP32-CAM-%s", suffix);
    return ESP_OK;
}
