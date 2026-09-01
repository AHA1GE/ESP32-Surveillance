#include "ws_stream.h"

#include "led.h"

#include <esp_log.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <stdio.h>
#include <string.h>

#define TAG "ws_stream"

/* How long a frame send may block before the frame is dropped. */
#define WS_SEND_TIMEOUT_MS 2000

/* Tracked by the event handler so the streaming task can drop frames while
 * the client is disconnected instead of failing a send every frame period. */
static volatile bool s_ws_connected;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                                    void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)handler_args;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            s_ws_connected = true;
            ESP_LOGI(TAG, "WebSocket connected");
            led_set_pattern(LED_PATTERN_SOLID);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            s_ws_connected = false;
            ESP_LOGW(TAG, "WebSocket disconnected");
            led_set_pattern(LED_PATTERN_FAST_FLASH);
            break;
        case WEBSOCKET_EVENT_DATA:
            ESP_LOGD(TAG, "Received data: len=%d", data->data_len);
            break;
        case WEBSOCKET_EVENT_ERROR:
            s_ws_connected = false;
            ESP_LOGE(TAG, "WebSocket error");
            led_set_pattern(LED_PATTERN_FAST_FLASH);
            break;
    }
}

esp_websocket_client_handle_t ws_stream_init(const char *device_id,
                                             const char *backend_host,
                                             uint16_t backend_port)
{
    char uri[256];
    snprintf(uri, sizeof(uri), "ws://%s:%u/ingest/%s", backend_host, backend_port, device_id);

    esp_websocket_client_config_t websocket_cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 10000,
        /* Send each camera frame as ONE WebSocket message. The 1KiB default
         * would fragment every JPEG into ~100 frames on the wire, which hits
         * a coder/websocket server-side edge case (mid-message EOF is
         * mistaken for a clean end) and kills the connection every few
         * seconds. 128KiB covers the largest SVGA q10 JPEG with margin. */
        .buffer_size = 128 * 1024,
    };

    esp_websocket_client_handle_t client = esp_websocket_client_init(&websocket_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return NULL;
    }

    // TODO(verify): Confirm esp_websocket_register_events() signature and availability.
    // May need to pass event handler via config struct instead or use esp_event_handler_register().
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);

    esp_err_t err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: 0x%x", err);
        esp_websocket_client_destroy(client);
        return NULL;
    }

    return client;
}

esp_err_t ws_stream_send_frame(esp_websocket_client_handle_t client, uint8_t *buf, size_t len)
{
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Drop the frame while disconnected: send_bin() would just fail and
     * log-spam at frame rate while the reconnect timer runs. The state
     * transitions are logged once each in the event handler instead. */
    if (!s_ws_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    /* A stalled TCP connection would otherwise block the streaming task
     * forever on portMAX_DELAY while it holds the camera frame buffer -
     * freezing the stream until reboot. Bound the wait and drop the frame
     * instead; the next capture retries and the stream recovers. */
    int ret = esp_websocket_client_send_bin(client, (char *)buf, len, pdMS_TO_TICKS(WS_SEND_TIMEOUT_MS));
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send frame: %d", ret);
        return ESP_FAIL;
    }

    return ESP_OK;
}
