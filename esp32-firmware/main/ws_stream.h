#pragma once

#include <esp_err.h>
#include <esp_websocket_client.h>
#include <stddef.h>
#include <stdint.h>

esp_websocket_client_handle_t ws_stream_init(const char *device_id,
                                             const char *backend_host,
                                             uint16_t backend_port);
esp_err_t ws_stream_send_frame(esp_websocket_client_handle_t client, uint8_t *buf, size_t len);
