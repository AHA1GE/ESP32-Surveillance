#pragma once

#include <esp_err.h>
#include <stdint.h>

/**
 * One-time setup: netif init, default event loop, esp_wifi_init with
 * nvs_enable=0 (the WiFi driver must NOT persist/auto-reconnect STA config,
 * or it would defeat portal mode), STA + AP default netifs, event handlers,
 * and the connect-signal event group.
 */
esp_err_t wifi_subsystem_init(void);

/**
 * Connect to `ssid` in STA mode, blocking up to connect_timeout_ms for an
 * IP address. ESP_OK on success, ESP_ERR_TIMEOUT if no IP within the timeout.
 */
esp_err_t wifi_start_sta(const char *ssid, const char *password, uint32_t connect_timeout_ms);

/** Start the config AP (open network, SSID ESP32-CAM-XXXXXX). */
esp_err_t wifi_start_ap(void);

/** Stop the WiFi driver (used when falling back from STA to the AP portal). */
esp_err_t wifi_stop(void);
