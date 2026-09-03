#include "lan_stream.h"

#include "camera.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mdns.h>
#include <stdio.h>
#include <string.h>

#define TAG "lan_stream"

#define PART_BOUNDARY       "frame"
#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" PART_BOUNDARY

static httpd_handle_t s_server;
/* Set by lan_stream_stop before httpd_stop: a handler stuck on a dead camera
 * (no frame ever arrives, so no send fails) exits on this flag - otherwise
 * httpd_stop would block forever waiting for the handler. */
static volatile bool s_stopping;

/* GET / - one MJPEG stream per request. Blocks the (single) httpd task for
 * the lifetime of the stream, which is also what limits serving to one
 * viewer. Frames are grabbed straight from the camera: latest frame only,
 * no copy, the Espressif local_jpeg_stream pattern. */
static esp_err_t mjpeg_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    static char part_buf[96];
    while (1) {
        camera_fb_t *fb = camera_capture();
        if (!fb) {
            /* Transient: both framebuffers busy (the streaming task grabs at
             * 10 fps too) or a driver hiccup - retry. Exits only when the
             * server is stopping, so a dead camera can't wedge httpd_stop. */
            if (s_stopping) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int n = snprintf(part_buf, sizeof(part_buf),
                         "--" PART_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                         (unsigned)fb->len);
        if (n < 0 || (size_t)n >= sizeof(part_buf)) {
            camera_release(fb);
            break;
        }
        /* A send failure means the client went away or the server is
         * stopping - end the stream either way. */
        if (httpd_resp_send_chunk(req, part_buf, n) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)fb->buf, (ssize_t)fb->len) != ESP_OK ||
            httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
            camera_release(fb);
            break;
        }
        camera_release(fb);
    }
    return ESP_OK;
}

/* Advertise <hostname>.local + an _http._tcp service on port 80. */
static esp_err_t lan_mdns_start(const char *hostname)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        return err;
    }
    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        mdns_free();
        return err;
    }
    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        mdns_free();
        return err;
    }
    return ESP_OK;
}

esp_err_t lan_stream_start(const char *hostname)
{
    if (s_server) {
        return ESP_OK; /* already running */
    }
    s_stopping = false;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 2;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: 0x%x", err);
        return err;
    }

    httpd_uri_t stream_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = mjpeg_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(server, &stream_uri);
    if (err != ESP_OK) {
        httpd_stop(server);
        return err;
    }

    err = lan_mdns_start(hostname);
    if (err != ESP_OK) {
        /* mDNS failing must not kill the stream - the browser can still use
         * the raw IP. */
        ESP_LOGW(TAG, "mDNS start failed (0x%x) - LAN stream not discoverable", err);
    }

    s_server = server;
    ESP_LOGI(TAG, "LAN MJPEG stream on http://%s.local (port 80)", hostname);
    return ESP_OK;
}

esp_err_t lan_stream_stop(void)
{
    s_stopping = true;
    esp_err_t err = ESP_OK;
    if (s_server) {
        err = httpd_stop(s_server);
        s_server = NULL;
    }
    mdns_free(); /* safe when mDNS never started */
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LAN MJPEG stream stopped");
    }
    return err;
}
