#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

#include "config_store.h"

/**
 * Start the WebRTC streaming subsystem: a long-lived signaling WebSocket to
 * the backend plus the PeerConnection machinery (esp_peer). The device is
 * always the answerer; it builds/tears down a session when viewers come and
 * go over the signaling socket.
 */
esp_err_t webrtc_stream_init(const device_config_t *cfg, const char *device_id);

/** Full-frame PSRAM staging buffer, or NULL if the allocation failed at boot. */
uint8_t *webrtc_stream_staging_buffer(void);
size_t webrtc_stream_staging_size(void);

/**
 * Send one JPEG over the data channel as chunked blobs
 * ([1B flag][4B big-endian len][payload], bit0 = Start, bit1 = End).
 *
 * Returns:
 *  ESP_OK                 - all chunks queued to the data channel
 *  ESP_ERR_INVALID_STATE  - no live session / data channel not open (drop)
 *  ESP_ERR_INVALID_SIZE   - garbage JPEG (no SOI/EOI markers)
 *  ESP_ERR_NO_MEM         - data channel buffer full mid-frame; the rest of
 *                           the frame is dropped, next frame resyncs
 *  ESP_FAIL               - send failed for any other reason
 */
esp_err_t webrtc_stream_send_frame(uint8_t *buf, size_t len);
