#include "web_portal.h"

#include "config_store.h"
#include "led.h"

#include <ctype.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "web_portal"

#define MAX_FORM_BODY 2048

/* Pure HTML, no JS, no client-side verification - validation is server-side
 * only. %s placeholders: ssid, pass, host, port, turn, checked-auto_flash. */
static const char FORM_TEMPLATE[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>ESP32-CAM Setup</title></head><body>"
    "<h1>ESP32-CAM Setup</h1>"
    "<form method=\"POST\" action=\"/save\">"
    "<label>WiFi SSID<br><input type=\"text\" name=\"ssid\" maxlength=\"32\" value=\"%s\"></label><br>"
    "<label>WiFi Password<br><input type=\"password\" name=\"pass\" maxlength=\"63\" value=\"%s\"></label><br>"
    "<label>Server Host<br><input type=\"text\" name=\"host\" maxlength=\"63\" value=\"%s\"></label><br>"
    "<label>Server Port<br><input type=\"number\" name=\"port\" min=\"1\" max=\"65535\" value=\"%lu\"></label><br>"
    "<label>TURN Server URL (optional)<br><input type=\"text\" name=\"turn\" maxlength=\"127\" value=\"%s\"></label><br>"
    "<label><input type=\"checkbox\" name=\"auto_flash\"%s> Auto-flash light (reserved, future use)</label><br>"
    "<input type=\"submit\" value=\"Save and Reboot\">"
    "</form></body></html>";

static const char SUCCESS_PAGE[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<title>Saved</title></head><body>"
    "<h1>Configuration saved</h1>"
    "<p>The camera will reboot and connect.</p>"
    "</body></html>";

static void html_escape(const char *src, char *dst, size_t dst_len)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 6 < dst_len; p++) {
        const char *repl = NULL;
        switch (*p) {
        case '&': repl = "&amp;"; break;
        case '<': repl = "&lt;"; break;
        case '>': repl = "&gt;"; break;
        case '"': repl = "&quot;"; break;
        case '\'': repl = "&#39;"; break;
        }
        if (repl) {
            size_t n = strlen(repl);
            memcpy(dst + o, repl, n);
            o += n;
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}

/* Handlers run on httpd's single server task, so static buffers are safe. */
static esp_err_t send_form(httpd_req_t *req, const device_config_t *cfg)
{
    static char page[4096];
    char ssid_esc[CONFIG_SSID_MAX_LEN * 6 + 1];
    char pass_esc[CONFIG_PASSWORD_MAX_LEN * 6 + 1];
    char host_esc[CONFIG_HOST_MAX_LEN * 6 + 1];
    char turn_esc[CONFIG_TURN_URL_MAX_LEN * 6 + 1];

    html_escape(cfg->wifi_ssid, ssid_esc, sizeof(ssid_esc));
    html_escape(cfg->wifi_password, pass_esc, sizeof(pass_esc));
    html_escape(cfg->backend_host, host_esc, sizeof(host_esc));
    html_escape(cfg->turn_server, turn_esc, sizeof(turn_esc));

    int n = snprintf(page, sizeof(page), FORM_TEMPLATE,
                     ssid_esc, pass_esc, host_esc,
                     (unsigned long)cfg->backend_port,
                     turn_esc,
                     cfg->auto_flash ? " checked" : "");
    if (n < 0 || (size_t)n >= sizeof(page)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Form too large");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, page);
    return ESP_OK;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode application/x-www-form-urlencoded ('+' -> space, %HH -> byte). */
static esp_err_t url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t o = 0;
    for (const char *p = src; *p; p++) {
        if (o >= dst_len - 1) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (*p == '+') {
            dst[o++] = ' ';
        } else if (*p == '%') {
            /* a trailing '%' (or % at end of string) is malformed - reject
             * before reading p[2] so a truncated pair can't over-read */
            if (p[1] == '\0' || p[2] == '\0') {
                return ESP_ERR_INVALID_ARG;
            }
            int hi = hexval(p[1]);
            int lo = hexval(p[2]);
            if (hi < 0 || lo < 0) {
                return ESP_ERR_INVALID_ARG;
            }
            dst[o++] = (char)((hi << 4) | lo);
            p += 2;
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
    return ESP_OK;
}

/* Split "k1=v1&k2=v2" into a device_config_t. Unknown keys are ignored;
 * checkbox keys (auto_flash) are true when present. Buffers are static:
 * handlers run on httpd's single server task, and MAX_FORM_BODY-sized locals
 * would blow the task stack. */
static esp_err_t parse_form(const char *body, device_config_t *cfg)
{
    static char buf[MAX_FORM_BODY + 1];
    static char val_dec[MAX_FORM_BODY + 1];
    strncpy(buf, body, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    for (char *pair = strtok_r(buf, "&", &saveptr); pair;
         pair = strtok_r(NULL, "&", &saveptr)) {
        char *eq = strchr(pair, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *raw_key = pair;
        char *raw_val = eq + 1;

        char key_dec[64];
        if (url_decode(raw_key, key_dec, sizeof(key_dec)) != ESP_OK ||
            url_decode(raw_val, val_dec, sizeof(val_dec)) != ESP_OK) {
            continue;
        }

        if (strcmp(key_dec, "ssid") == 0) {
            strlcpy(cfg->wifi_ssid, val_dec, sizeof(cfg->wifi_ssid));
        } else if (strcmp(key_dec, "pass") == 0) {
            strlcpy(cfg->wifi_password, val_dec, sizeof(cfg->wifi_password));
        } else if (strcmp(key_dec, "host") == 0) {
            strlcpy(cfg->backend_host, val_dec, sizeof(cfg->backend_host));
        } else if (strcmp(key_dec, "port") == 0) {
            char *end = NULL;
            unsigned long p = strtoul(val_dec, &end, 10);
            /* a bad port becomes 0, which config_validate rejects */
            cfg->backend_port = (p >= 1 && p <= 65535 && end && *end == '\0') ? (uint32_t)p : 0;
        } else if (strcmp(key_dec, "turn") == 0) {
            strlcpy(cfg->turn_server, val_dec, sizeof(cfg->turn_server));
        } else if (strcmp(key_dec, "auto_flash") == 0) {
            cfg->auto_flash = true;
        }
    }
    return ESP_OK;
}

/* GET / - serve the form prefilled with the stored config (best effort). */
static esp_err_t form_handler(httpd_req_t *req)
{
    device_config_t cfg;
    config_init_defaults(&cfg);
    config_store_load(&cfg); /* ignore result: defaults are a fine prefill */
    return send_form(req, &cfg);
}

/* POST /save - parse, validate, store, reboot; invalid submits are silently
 * ignored (form re-served with the submitted values, no error message). */
static esp_err_t save_handler(httpd_req_t *req)
{
    if (req->content_len > MAX_FORM_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }

    static char body[MAX_FORM_BODY + 1];
    int total = 0;
    while (total < (int)req->content_len) {
        int received = httpd_req_recv(req, body + total, req->content_len - total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
            return ESP_FAIL;
        }
        total += received;
    }
    body[total] = '\0';

    device_config_t cfg;
    config_init_defaults(&cfg);
    if (parse_form(body, &cfg) != ESP_OK || config_validate(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid config submit ignored");
        led_error_flash(3);
        return send_form(req, &cfg); /* stay in portal, silently */
    }

    esp_err_t err = config_store_save(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: 0x%x", err);
        led_error_flash(3);
        return send_form(req, &cfg);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, SUCCESS_PAGE);
    ESP_LOGI(TAG, "Config saved, rebooting in 2s");
    vTaskDelay(pdMS_TO_TICKS(2000)); /* let the browser render the page */
    esp_restart();
    return ESP_OK;
}

esp_err_t web_portal_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    /* send_form's escape buffers are ~1.8KB of stack; the default 4096-byte
     * task stack left little margin once the form grew. */
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t form_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = form_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &form_uri);
    httpd_register_uri_handler(server, &save_uri);

    ESP_LOGI(TAG, "Config portal listening on port %d", config.server_port);
    return ESP_OK;
}
