#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "wifi.h"
#include "camera.h"
#include "ws_stream.h"
#include "ota.h"

#define TAG "app"

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

    ESP_LOGI(TAG, "WiFi connecting...");
    ret = wifi_init_sta();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi initialization failed: 0x%x", ret);
    }

    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "Camera initializing...");
    ret = camera_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera initialization failed: 0x%x", ret);
    }

    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "WebSocket client connecting...");
    esp_websocket_client_handle_t ws_client = ws_stream_init(CONFIG_DEVICE_ID);
    if (!ws_client) {
        ESP_LOGE(TAG, "WebSocket client initialization failed");
    }

    vTaskDelay(pdMS_TO_TICKS(2000));

    xTaskCreatePinnedToCore(streaming_task, "streaming", 4096, ws_client, 5, NULL, 1);

    if (CONFIG_ENABLE_AUTO_OTA) {
        ESP_LOGI(TAG, "OTA is enabled, but auto-check is not yet implemented");
        ESP_LOGI(TAG, "To use OTA: call ota_check_latest_release() periodically from a timer");

        ota_release_info_t release_info = {0};
        esp_err_t ota_err = ota_check_latest_release(CONFIG_OTA_GITHUB_OWNER, CONFIG_OTA_GITHUB_REPO, &release_info);
        if (ota_err == ESP_OK && release_info.update_available) {
            ESP_LOGI(TAG, "Update available: %s", release_info.tag_name);
            ESP_LOGI(TAG, "URL: %s", release_info.url);
        }
    }
}
