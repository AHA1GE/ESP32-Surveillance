#include "wifi.h"

#include "device_id.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <string.h>

#define TAG "wifi"

#define WIFI_BIT_GOT_IP BIT0

static EventGroupHandle_t s_wifi_evt_group;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "AP client connected");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_evt_group) {
            xEventGroupSetBits(s_wifi_evt_group, WIFI_BIT_GOT_IP);
        }
    }
}

esp_err_t wifi_subsystem_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK) return err;

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* The WiFi driver would otherwise persist the STA config to its own NVS
     * namespace and auto-reconnect at next boot, defeating portal mode. */
    cfg.nvs_enable = 0;
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (err != ESP_OK) return err;

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (err != ESP_OK) return err;

    s_wifi_evt_group = xEventGroupCreate();
    if (!s_wifi_evt_group) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t wifi_start_sta(const char *ssid, const char *password, uint32_t connect_timeout_ms)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Runtime creds must be copied into the fixed-size config structs
     * (strlcpy, not the old CONFIG_* compound-literal trick). */
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    xEventGroupClearBits(s_wifi_evt_group, WIFI_BIT_GOT_IP);

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    /* WIFI_EVENT_STA_START fires esp_wifi_connect() via the event handler. */

    /* The default modem power save naps the radio between DTIM beacons,
     * which stalls WebSocket frame sends and collapses TCP throughput.
     * Streaming needs the radio on continuously. */
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable WiFi power save: 0x%x", err);
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt_group, WIFI_BIT_GOT_IP,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(connect_timeout_ms));
    if ((bits & WIFI_BIT_GOT_IP) == 0) {
        ESP_LOGW(TAG, "STA connect timeout (%lu ms)", (unsigned long)connect_timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t wifi_start_ap(void)
{
    char ap_ssid[32];
    if (device_id_get_ap_ssid(ap_ssid, sizeof(ap_ssid)) != ESP_OK) {
        return ESP_FAIL;
    }

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(ap_ssid);
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.channel = 1;

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Config AP '%s' started (open)", ap_ssid);
    return ESP_OK;
}

esp_err_t wifi_stop(void)
{
    return esp_wifi_stop();
}
