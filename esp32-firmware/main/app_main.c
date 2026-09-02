#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "wifi.h"
#include "camera.h"
#include "webrtc_stream.h"
#include "ota.h"
#include "config_store.h"
#include "device_id.h"
#include "led.h"
#include "web_portal.h"

#define TAG "app"

#define STA_CONNECT_TIMEOUT_MS    60000
#define CAMERA_RETRY_INTERVAL_MS  5000

/* Pacing ceiling. The camera produces new VGA frames at roughly the sensor
 * rate (community baseline SVGA ~6fps scales to ~9-10fps at VGA); grab_latest
 * makes each grab the newest completed frame, so the loop sends every real
 * frame once and pacing to 10fps only bounds the load if the sensor runs
 * ahead. */
#define STREAM_TARGET_FPS          10

static void streaming_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Starting streaming task");

    const TickType_t frame_period = pdMS_TO_TICKS(1000 / STREAM_TARGET_FPS);
    TickType_t last_wake = xTaskGetTickCount();

    /* Rolling telemetry over a ~10s window, measured in tick time so a slow
     * window (send stalls, reconnect) reports a true rate rather than the
     * loop's attempted rate. */
    const TickType_t stats_period = pdMS_TO_TICKS(10000);
    TickType_t stats_window_start = xTaskGetTickCount();
    uint32_t frames_captured = 0;
    uint32_t frames_sent = 0;
    uint32_t frames_failed = 0;
    uint32_t frames_invalid = 0;
    uint32_t frames_dropped = 0;
    uint64_t last_capture_us = 0;

    while (1) {
        camera_fb_t *fb = camera_capture();
        if (fb) {
            /* Count genuinely new frames: grab_latest normally guarantees a
             * fresh timestamp per grab, so this only filters duplicates. */
            uint64_t capture_us = (uint64_t)fb->timestamp.tv_sec * 1000000UL + fb->timestamp.tv_usec;
            if (capture_us != last_capture_us) {
                frames_captured++;
                last_capture_us = capture_us;
            }

            size_t frame_len = fb->len;
            uint8_t *staging = webrtc_stream_staging_buffer();
            esp_err_t err;
            if (staging && frame_len <= webrtc_stream_staging_size()) {
                /* Copy the JPEG out of the framebuffer and release it before
                 * the (blocking) send, so the camera keeps capturing while
                 * this frame drains over the datachannel. */
                memcpy(staging, fb->buf, frame_len);
                camera_release(fb);
                err = webrtc_stream_send_frame(staging, frame_len);
            } else {
                /* No staging (allocation failed at boot, or an oversized
                 * frame): fall back to holding the framebuffer for the whole
                 * send, the pre-staging behavior. */
                err = webrtc_stream_send_frame(fb->buf, frame_len);
                camera_release(fb);
            }

            if (err == ESP_OK) {
                frames_sent++;
            } else if (err == ESP_FAIL) {
                frames_failed++;
            } else if (err == ESP_ERR_INVALID_SIZE) {
                frames_invalid++;
            } else if (err == ESP_ERR_INVALID_STATE) {
                /* No datachannel (no viewer) - frame intentionally not sent. */
                frames_dropped++;
            } else if (err == ESP_ERR_NO_MEM) {
                /* WOULD_BLOCK: datachannel send cache full. The frame is
                 * dropped whole and the next one resyncs on its Start flag. */
                frames_dropped++;
            }
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - stats_window_start) >= stats_period) {
            float elapsed_s = (float)(now - stats_window_start) * portTICK_PERIOD_MS / 1000.0f;
            ESP_LOGI(TAG, "stream: capture=%.1ffps sent=%.1ffps failed=%lu invalid=%lu dropped=%lu heap_free=%u",
                     frames_captured / elapsed_s, frames_sent / elapsed_s,
                     (unsigned long)frames_failed, (unsigned long)frames_invalid,
                     (unsigned long)frames_dropped,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            stats_window_start = now;
            frames_captured = 0;
            frames_sent = 0;
            frames_failed = 0;
            frames_invalid = 0;
            frames_dropped = 0;
        }

        vTaskDelayUntil(&last_wake, frame_period);
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

    ESP_LOGI(TAG, "WebRTC signaling connecting...");
    while (webrtc_stream_init(cfg, device_id) != ESP_OK) {
        ESP_LOGE(TAG, "WebRTC stream init failed, retrying in 10s");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    xTaskCreatePinnedToCore(streaming_task, "streaming", 4096, NULL, 5, NULL, 1);

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
