#pragma once

#include <esp_err.h>

/**
 * Start the LAN fallback stream: an HTTP server on port 80 serving the camera
 * as MJPEG (`multipart/x-mixed-replace`) at "/", advertised via mDNS as
 * `<hostname>.local`. Pass the device ID ("esp32cam-XXXXXX") as hostname.
 *
 * Single viewer by construction: the handler blocks the (single) httpd task
 * for the lifetime of a stream, so a second client is only served after the
 * first disconnects. Frames come straight from the camera (latest frame, the
 * Espressif local_jpeg_stream pattern); the handler is the sole consumer, so
 * no per-frame copy is needed.
 *
 * Returns ESP_OK if already running.
 */
esp_err_t lan_stream_start(const char *hostname);

/**
 * Stop the HTTP server and mDNS. Safe to call when nothing is running
 * (returns ESP_OK); blocks until the in-flight MJPEG handler returns.
 */
esp_err_t lan_stream_stop(void);
