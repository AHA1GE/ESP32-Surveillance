#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "wifi.h"
#include "camera.h"
#include "ws_stream.h"
#include "ota.h"
#include "config_store.h"
#include "device_id.h"
#include "led.h"
#include "web_portal.h"

#define TAG "app"

#define STA_CONNECT_TIMEOUT_MS    60000
#define CAMERA_RETRY_INTERVAL_MS  5000

static void streaming_task(void *pvParameters)
{
    esp_websocket_client_handle_t ws_client = (esp_websocket_client_handle_t)pvParameters;

    ESP_LOGI(TAG, "Starting streaming task");

    while (1) {
        camera_fb_t *fb = camera_capture();
        if (fb) {
            ws_stream_send_frame(ws_client, fb->buf, fb->len);
            camera_release(fb);
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    vTaskDelete(NULL);
}

/* Reboot is the only exit - a valid config save restarts into normal mode. */
static void enter_portal_mode(void)
{
    ESP_LOGW(TAG, "Entering config portal mode");
    led_set_pattern(LED_PATTERN_DOUBLE_BLINK);

    if (wifi_start_ap() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start config AP");
    }
    if (web_portal_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web portal");
    }

    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}

static void run_normal_mode(const device_config_t *cfg)
{
    ESP_LOGI(TAG, "Normal mode: backend %s:%lu",
             cfg->backend_host, (unsigned long)cfg->backend_port);

    /* "backend not connected" until the WebSocket reports CONNECTED */
    led_set_pattern(LED_PATTERN_FAST_FLASH);

    if (wifi_start_sta(cfg->wifi_ssid, cfg->wifi_password, STA_CONNECT_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "STA connect failed, falling back to portal");
        wifi_stop();
        enter_portal_mode();
    }

    // A disabled bool Kconfig option isn't defined as 0 - it's not defined at
    // all - so this has to be a preprocessor check, not a runtime "if".
#if CONFIG_ENABLE_AUTO_OTA
    ESP_LOGI(TAG, "OTA is enabled, but auto-check is not yet implemented");
    ESP_LOGI(TAG, "To use OTA: call ota_check_latest_release() periodically from a timer");

    ota_release_info_t release_info = {0};
    esp_err_t ota_err = ota_check_latest_release(CONFIG_OTA_GITHUB_OWNER, CONFIG_OTA_GITHUB_REPO, &release_info);
    if (ota_err == ESP_OK && release_info.update_available) {
        ESP_LOGI(TAG, "Update available: %s", release_info.tag_name);
        ESP_LOGI(TAG, "URL: %s", release_info.url);
    }
#endif

    /* Camera init failure is a hardware problem - retry in normal mode with
     * the error LED, never drop to the portal. */
    ESP_LOGI(TAG, "Camera initializing...");
    while (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed, retrying in %d ms", CAMERA_RETRY_INTERVAL_MS);
        led_error_flash(2);
        vTaskDelay(pdMS_TO_TICKS(CAMERA_RETRY_INTERVAL_MS));
    }

    char device_id[16];
    device_id_get(device_id, sizeof(device_id));
    ESP_LOGI(TAG, "Device ID: %s", device_id);

    ESP_LOGI(TAG, "WebSocket client connecting...");
    esp_websocket_client_handle_t ws_client = NULL;
    while (!ws_client) {
        ws_client = ws_stream_init(device_id, cfg->backend_host, (uint16_t)cfg->backend_port);
        if (!ws_client) {
            ESP_LOGE(TAG, "WebSocket client init failed, retrying in 10s");
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    xTaskCreatePinnedToCore(streaming_task, "streaming", 4096, ws_client, 5, NULL, 1);

    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 Surveillance Firmware");
    ESP_LOGI(TAG, "App version: %s", APP_VERSION);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(led_init());

    /* Both modes need WiFi: the portal path starts an AP, so the subsystem
     * must be up before the config-load branch below. */
    ESP_ERROR_CHECK(wifi_subsystem_init());

    device_config_t cfg;
    ret = config_store_load(&cfg);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "No device config found - first flash or erased NVS");
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Stored config invalid - entering portal");
        } else {
            ESP_LOGE(TAG, "Config load error 0x%x - entering portal", ret);
        }
        enter_portal_mode();
    }

    run_normal_mode(&cfg);
}
