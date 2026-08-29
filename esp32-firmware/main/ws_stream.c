#include "ws_stream.h"

#include <esp_log.h>
#include <esp_websocket_client.h>
#include <string.h>
#include <stdio.h>

#define TAG "ws_stream"

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                                   void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)handler_args;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected");
            break;
        case WEBSOCKET_EVENT_DATA:
            ESP_LOGD(TAG, "Received data: len=%d", data->data_len);
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            break;
    }
}

esp_websocket_client_handle_t ws_stream_init(const char *device_id)
{
    char uri[256];
    snprintf(uri, sizeof(uri), "ws://%s:%d/ingest/%s",
             CONFIG_BACKEND_HOST, CONFIG_BACKEND_PORT, device_id);

    esp_websocket_client_config_t websocket_cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 10000,
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

    int ret = esp_websocket_client_send_bin(client, (char *)buf, len, portMAX_DELAY);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send frame: %d", ret);
        return ESP_FAIL;
    }

    return ESP_OK;
}
