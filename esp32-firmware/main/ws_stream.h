#pragma once

#include <esp_err.h>
#include <esp_websocket_client.h>
#include <stddef.h>
#include <stdint.h>

esp_websocket_client_handle_t ws_stream_init(const char *device_id,
                                             const char *backend_host,
                                             uint16_t backend_port);
esp_err_t ws_stream_send_frame(esp_websocket_client_handle_t client, uint8_t *buf, size_t len);

/* PSRAM staging buffer: the streaming task copies a captured frame into it and
 * releases the camera framebuffer before the (blocking) send, so capture keeps
 * running while the frame drains over WiFi. NULL if allocation failed at init. */
uint8_t *ws_stream_staging_buffer(void);
size_t ws_stream_staging_size(void);
