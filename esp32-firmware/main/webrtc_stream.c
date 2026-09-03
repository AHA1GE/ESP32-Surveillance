#include "webrtc_stream.h"

#include "lan_stream.h"
#include "led.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_peer.h>
#include <esp_timer.h>
#include <esp_peer_default.h>
#include <esp_tls.h>
#include <esp_transport.h>
#include <esp_transport_ssl.h>
#include <esp_transport_tcp.h>
#include <esp_transport_ws.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#define TAG "webrtc_stream"

/* Chunked JPEG wire format: payload chunks of at most this many bytes,
 * framed as [1B flag][4B big-endian length][payload]. The browser side of
 * the protocol is view.html. */
#define CHUNK_PAYLOAD_SIZE  10000
#define CHUNK_WIRE_SIZE     (CHUNK_PAYLOAD_SIZE + 5)
#define CHUNK_FLAG_START    0x01
#define CHUNK_FLAG_END      0x02

/* How long one signaling send (answer SDP / candidate) may block. */
#define WS_SEND_TIMEOUT_MS  2000

/* Answer SDPs and candidate lines are tiny (a browser offer is < 6KB even
 * with embedded candidates); 8KB is generous and keeps the static buffer
 * off the heap. */
#define MSG_BUF_SIZE        8192

/* VGA JPEGs are capped at ~60KiB by the camera driver's width*height/5 fb
 * allocation, so 64KiB always holds a full frame. PSRAM, same rationale as
 * the old ws_stream staging buffer. */
#define STAGING_SIZE        (64 * 1024)

/* ICE servers: backend-provided STUN + TURN (max 2 STUN entries, one TURN
 * triple) plus headroom. URLs are copied into static storage because
 * esp_peer keeps the pointers. Cloudflare TURN usernames are 128 hex chars,
 * so credentials need 128 + NUL. */
#define MAX_ICE_SERVERS     4
#define ICE_URL_MAX         127
#define ICE_CRED_MAX        128

#define PEER_TASK_STACK     (12 * 1024)
#define PEER_LOOP_PERIOD_MS 10

/* App-level ping: the Worker expires idle device slots by message traffic,
 * not TCP liveness, so ping every 30 s while connected. (The library-level
 * WS pings answer the edge's protocol pings automatically.) Wall clock, not
 * peer-loop ticks - main_loop's duration can stretch, and the Worker's 90 s
 * idle window does not tolerate a stretched ping interval. */
#define APP_PING_INTERVAL_MS    30000
/* Offline for ~60 s: the cloud is unreachable, start the LAN MJPEG fallback
 * (the ws client keeps retrying every 3 s underneath). Wall clock too, so a
 * slow peer loop can't stretch the threshold into hours. */
#define LAN_FALLBACK_MS         60000

#define DC_LABEL            "video_data"

static esp_websocket_client_handle_t s_ws;
/* Signaling transports, built by us in webrtc_stream_init. Production wss:
 * the parent is an esp-tls transport with the addr_family pinned to IPv4 so
 * the backend hostname resolves over IPv4 only (see the pin below); local
 * dev (ws://): the parent is plain TCP. The ws client never frees an
 * ext_transport, so these handles are ours to destroy - and ws_destroy
 * does not free its parent, so both are kept. */
static esp_transport_handle_t s_ws_transport;
static esp_transport_handle_t s_ws_parent_transport;
static esp_peer_handle_t s_peer;
static uint8_t *s_staging;
static uint8_t *s_chunk; /* PSRAM: one wire-frame chunk, reused per send */

/* Device ID (own copy): the peer task needs it for the LAN fallback's mDNS
 * hostname, and the caller's buffer is not guaranteed to outlive init. */
static char s_device_id[16];

/* Cross-task state. on_state runs on the peer task (inside main_loop),
 * the ws event handler on the websocket task, and send_frame on the
 * streaming task - so these are all volatile. */
static volatile bool s_dc_open;
static volatile bool s_ws_connected;
/* ms timestamp of the last ws disconnect, set by the ws event handler; the
 * peer task compares it against the wall clock for the LAN fallback. */
static volatile uint32_t s_disconnected_at_ms;
/* Set by the peer task while the LAN stream is serving: the ws handler must
 * not repaint the LED back to FAST_FLASH while LAN mode is active - cloud
 * errors are the whole reason the mode exists. */
static volatile bool s_lan_active;
static volatile esp_peer_state_t s_peer_state = ESP_PEER_STATE_DISCONNECTED;

/* ICE server storage, filled when an offer (with attached STUN/TURN)
 * arrives and reused for the lifetime of the peer. */
static esp_peer_ice_server_cfg_t s_ice_servers[MAX_ICE_SERVERS];
static char s_ice_urls[MAX_ICE_SERVERS][ICE_URL_MAX + 1];
static char s_ice_user[ICE_CRED_MAX + 1];
static char s_ice_psw[ICE_CRED_MAX + 1];

static int on_peer_state(esp_peer_state_t state, void *ctx)
{
    s_peer_state = state;
    switch (state) {
        case ESP_PEER_STATE_CONNECTED:
            ESP_LOGI(TAG, "Peer connected");
            break;
        case ESP_PEER_STATE_DATA_CHANNEL_CONNECTED: {
            /* SCTP is up: create the unordered video channel. The browser
             * side accepts it via ondatachannel (view.html). */
            esp_peer_data_channel_cfg_t ch_cfg = {
                .type = ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_RETX,
                .ordered = false,
                .label = DC_LABEL,
                .max_retransmit_count = 1,
            };
            int ret = esp_peer_create_data_channel(s_peer, &ch_cfg);
            if (ret != ESP_PEER_ERR_NONE) {
                ESP_LOGE(TAG, "create data channel failed: %d", ret);
            }
            break;
        }
        case ESP_PEER_STATE_DATA_CHANNEL_OPENED:
            s_dc_open = true;
            ESP_LOGI(TAG, "Data channel opened");
            break;
        case ESP_PEER_STATE_DISCONNECTED:
        case ESP_PEER_STATE_CONNECT_FAILED:
        case ESP_PEER_STATE_DATA_CHANNEL_CLOSED:
            s_dc_open = false;
            ESP_LOGW(TAG, "Peer state %d - session over", (int)state);
            break;
        default:
            ESP_LOGD(TAG, "Peer state %d", (int)state);
            break;
    }
    return 0;
}

/* Fires on the peer task for every message esp_peer generates for the
 * remote side: our answer SDP and our ICE candidates. Forward them over
 * the signaling socket as JSON. A single static buffer is enough - the
 * callback only ever runs inside esp_peer_main_loop(). */
static int on_peer_msg(esp_peer_msg_t *msg, void *ctx)
{
    if (!s_ws) {
        return 0;
    }
    if (msg->size <= 0 || (size_t)msg->size >= MSG_BUF_SIZE) {
        ESP_LOGE(TAG, "peer msg too big: %d", msg->size);
        return 0;
    }

    /* cJSON copies its inputs, so copy out of the callback-scoped buffer. */
    static char buf[MSG_BUF_SIZE];
    memcpy(buf, msg->data, msg->size);
    buf[msg->size] = '\0';

    cJSON *root = cJSON_CreateObject();
    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        cJSON_AddStringToObject(root, "type", "answer");
        cJSON_AddStringToObject(root, "sdp", buf);
    } else if (msg->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
        cJSON_AddStringToObject(root, "type", "ice");
        cJSON_AddStringToObject(root, "candidate", buf);
    } else {
        cJSON_Delete(root);
        return 0;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return 0;
    }
    esp_err_t err = esp_websocket_client_send_text(
        s_ws, json, (int)strlen(json), pdMS_TO_TICKS(WS_SEND_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "forward %s to backend failed: 0x%x",
                 msg->type == ESP_PEER_MSG_TYPE_SDP ? "answer" : "ice", err);
    }
    free(json);
    return 0;
}

static int on_peer_data(esp_peer_data_frame_t *frame, void *ctx)
{
    /* Send-only device; an incoming frame is unexpected (and harmless). */
    ESP_LOGW(TAG, "unexpected data frame: %d bytes", frame->size);
    return 0;
}

/* The LED shows the system mode: solid = cloud connected, slow blink = LAN
 * fallback serving, fast flash = error. Single owner function so the ws
 * handler's error events can't clobber SLOW_BLINK - they fire on every
 * failed reconnect attempt, which is the norm in LAN mode. */
static void led_update(void)
{
    if (s_ws_connected) {
        led_set_pattern(LED_PATTERN_SOLID);
    } else if (s_lan_active) {
        led_set_pattern(LED_PATTERN_SLOW_BLINK);
    } else {
        led_set_pattern(LED_PATTERN_FAST_FLASH);
    }
}

static void peer_task(void *arg)
{
    uint32_t last_ping_ms = 0;
    bool lan_up = false;

    while (s_peer) {
        esp_peer_main_loop(s_peer);
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        if (s_ws_connected) {
            /* Unsigned math handles the 49-day wrap. */
            if (now_ms - last_ping_ms >= APP_PING_INTERVAL_MS) {
                last_ping_ms = now_ms;
                static const char ping[] = "{\"type\":\"ping\"}";
                esp_err_t err = esp_websocket_client_send_text(
                    s_ws, ping, sizeof(ping) - 1, pdMS_TO_TICKS(WS_SEND_TIMEOUT_MS));
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "ping send failed: 0x%x", err);
                    /* A failed ping can mean the client lost the connection
                     * without our handler seeing the event (the Worker closes
                     * cleanly on idle-kill/redeploy and the old code never
                     * handled that). Ask the library itself: if it is not
                     * connected, s_ws_connected is stale and the fallback
                     * must engage. Field-verified 2026-09-03: the device
                     * pinged a dead connection for 30+ minutes while the LAN
                     * fallback never started. */
                    if (!esp_websocket_client_is_connected(s_ws)) {
                        s_ws_connected = false;
                        s_disconnected_at_ms = now_ms;
                        led_update();
                    }
                }
            }
            if (lan_up) {
                /* Cloud is back - stop serving the LAN stream. (The ws
                 * event handler already switched the LED to solid.) */
                lan_stream_stop();
                lan_up = false;
                s_lan_active = false;
            }
        } else if (!lan_up && now_ms - s_disconnected_at_ms >= LAN_FALLBACK_MS) {
            if (lan_stream_start(s_device_id) == ESP_OK) {
                lan_up = true;
                s_lan_active = true;
                led_update();
            } else {
                /* Back off for another full window instead of retrying
                 * every loop tick (httpd_start failures log every time). */
                s_disconnected_at_ms = now_ms;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PEER_LOOP_PERIOD_MS));
    }
    vTaskDelete(NULL);
}

/* Copy a NUL-terminated string into static ICE storage, truncating rather
 * than overrunning; a truncated URL just fails its own connectivity check
 * while the rest of the candidates still work. */
static bool copy_ice_url(char *dst, const char *src)
{
    size_t len = strlen(src);
    if (len == 0 || len > ICE_URL_MAX) {
        return false;
    }
    memcpy(dst, src, len + 1);
    return true;
}

/* Rebuild the ICE server table from an offer's attached stun[]/turn{}. */
static int build_ice_servers(const cJSON *stun, const cJSON *turn)
{
    int n = 0;

    if (cJSON_IsArray(stun)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, stun) {
            if (n >= MAX_ICE_SERVERS - 1 || !cJSON_IsString(item)) {
                break;
            }
            if (copy_ice_url(s_ice_urls[n], item->valuestring)) {
                s_ice_servers[n].stun_url = s_ice_urls[n];
                s_ice_servers[n].user = NULL;
                s_ice_servers[n].psw = NULL;
                n++;
            }
        }
    }

    if (cJSON_IsObject(turn)) {
        const cJSON *urls = cJSON_GetObjectItem(turn, "urls");
        if (cJSON_IsArray(urls) && cJSON_GetArraySize(urls) > 0) {
            /* Prefer the UDP URL; the backend lists udp first, but pick it
             * explicitly so the order change can't silently break the relay
             * (esp_peer has no TCP ICE support in this build). */
            const cJSON *picked = NULL;
            cJSON *url = NULL;
            cJSON_ArrayForEach(url, urls) {
                if (!cJSON_IsString(url)) {
                    continue;
                }
                if (picked == NULL || strstr(url->valuestring, "transport=udp") != NULL) {
                    picked = url;
                }
                if (strstr(url->valuestring, "transport=udp") != NULL) {
                    break;
                }
            }
            const cJSON *user = cJSON_GetObjectItem(turn, "username");
            const cJSON *psw = cJSON_GetObjectItem(turn, "credential");
            if (picked != NULL && cJSON_IsString(user) && cJSON_IsString(psw) &&
                n < MAX_ICE_SERVERS) {
                if (copy_ice_url(s_ice_urls[n], picked->valuestring)) {
                    snprintf(s_ice_user, sizeof(s_ice_user), "%s", user->valuestring);
                    snprintf(s_ice_psw, sizeof(s_ice_psw), "%s", psw->valuestring);
                    s_ice_servers[n].stun_url = s_ice_urls[n];
                    s_ice_servers[n].user = s_ice_user;
                    s_ice_servers[n].psw = s_ice_psw;
                    n++;
                }
            }
        }
    }

    return n;
}

/* Feed "a=candidate:" lines embedded in a received SDP as separate
 * candidate messages (mirrors espressif's local_jpeg_stream, which does
 * the same because browsers with non-trickle ICE pack candidates into the
 * offer instead of trickling them). */
static void feed_embedded_candidates(const char *sdp)
{
    const char *p = sdp;
    while ((p = strstr(p, "a=candidate:")) != NULL) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == '\n')) {
            len--;
        }
        if (len > 0 && len < MSG_BUF_SIZE) {
            static char cand[MSG_BUF_SIZE];
            memcpy(cand, p, len);
            cand[len] = '\0';
            esp_peer_msg_t cand_msg = {
                .type = ESP_PEER_MSG_TYPE_CANDIDATE,
                .data = (uint8_t *)cand,
                .size = (int)len,
            };
            esp_peer_send_msg(s_peer, &cand_msg);
        }
        p += 11;
    }
}

/* A viewer arrived: (re)start the session as answerer and hand esp_peer
 * the browser's offer. The attached STUN/TURN replaces whatever a previous
 * session configured (credentials are minted per session). */
static void handle_offer(const cJSON *root)
{
    const cJSON *sdp = cJSON_GetObjectItem(root, "sdp");
    if (!cJSON_IsString(sdp)) {
        ESP_LOGE(TAG, "offer without sdp");
        return;
    }

    /* A stale session (viewer reconnected without a clean viewer_gone)
     * can't answer twice; tear it down first. */
    if (s_peer_state != ESP_PEER_STATE_DISCONNECTED &&
        s_peer_state != ESP_PEER_STATE_CLOSED &&
        s_peer_state != ESP_PEER_STATE_NEW_CONNECTION) {
        ESP_LOGW(TAG, "offer while in state %d - disconnecting stale session",
                 (int)s_peer_state);
        esp_peer_disconnect(s_peer);
    }

    int num = build_ice_servers(cJSON_GetObjectItem(root, "stun"),
                                cJSON_GetObjectItem(root, "turn"));
    int ret = esp_peer_update_ice_info(s_peer, ESP_PEER_ROLE_CONTROLLED,
                                       num > 0 ? s_ice_servers : NULL, num);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "update_ice_info failed: %d", ret);
    }

    ret = esp_peer_new_connection(s_peer);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "new_connection failed: %d", ret);
        return;
    }

    esp_peer_msg_t msg = {
        .type = ESP_PEER_MSG_TYPE_SDP,
        .data = (uint8_t *)sdp->valuestring,
        .size = (int)strlen(sdp->valuestring),
    };
    ret = esp_peer_send_msg(s_peer, &msg);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "send offer to peer failed: %d", ret);
        return;
    }
    ESP_LOGI(TAG, "Offer fed to peer (%d bytes, %d ICE servers)",
             msg.size, num);

    feed_embedded_candidates(sdp->valuestring);
}

static void handle_ws_message(const char *data, int len)
{
    /* esp_websocket_client delivers text frames NUL-terminated in practice,
     * but copy to be safe: cJSON_Parse needs a terminator. */
    char *copy = malloc(len + 1);
    if (!copy) {
        ESP_LOGE(TAG, "no heap for ws message (%d)", len);
        return;
    }
    memcpy(copy, data, len);
    copy[len] = '\0';

    cJSON *root = cJSON_Parse(copy);
    free(copy);
    if (!root) {
        ESP_LOGW(TAG, "unparseable signaling JSON");
        return;
    }

    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "offer") == 0) {
        handle_offer(root);
    } else if (strcmp(type->valuestring, "ice") == 0) {
        /* Browser candidate -> esp_peer. */
        const cJSON *cand = cJSON_GetObjectItem(root, "candidate");
        if (cJSON_IsString(cand)) {
            esp_peer_msg_t msg = {
                .type = ESP_PEER_MSG_TYPE_CANDIDATE,
                .data = (uint8_t *)cand->valuestring,
                .size = (int)strlen(cand->valuestring),
            };
            int ret = esp_peer_send_msg(s_peer, &msg);
            if (ret != ESP_PEER_ERR_NONE) {
                ESP_LOGD(TAG, "candidate rejected: %d", ret);
            }
        }
    } else if (strcmp(type->valuestring, "viewer_gone") == 0) {
        ESP_LOGI(TAG, "Viewer left - tearing down session");
        esp_peer_disconnect(s_peer);
        s_dc_open = false;
    }
    /* Unknown types are ignored; the protocol grows forward. */

    cJSON_Delete(root);
}

/* Mark signaling as gone. The LAN-fallback countdown starts only on the
 * transition out of "connected": a retry storm fires ERROR/DISCONNECTED
 * every ~13 s (one per connect timeout) and re-stamping on each of them
 * would keep pushing the LAN_FALLBACK_MS window out forever - the fallback
 * would never engage (field-verified 2026-09-03). */
static void mark_ws_disconnected(void)
{
    if (s_ws_connected) {
        s_disconnected_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    s_ws_connected = false;
    led_update();
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                                    void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Signaling WebSocket connected");
            s_ws_connected = true;
            led_update();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Signaling WebSocket disconnected");
            mark_ws_disconnected();
            esp_peer_disconnect(s_peer);
            s_dc_open = false;
            break;
        case WEBSOCKET_EVENT_CLOSED:
            /* Server-initiated close (the Worker's 90 s idle kill, or a
             * redeploy): same app-level effect as a drop - signaling is
             * gone, so stop the peer session and let the fallback timer
             * run. The client's own reconnect is governed by
             * enable_close_reconnect in the config below. */
            ESP_LOGW(TAG, "Signaling WebSocket closed by server");
            mark_ws_disconnected();
            esp_peer_disconnect(s_peer);
            s_dc_open = false;
            break;
        case WEBSOCKET_EVENT_DATA:
            /* One event carries a whole signaling message: buffer_size (8KB)
             * exceeds the largest SDP/JSON the backend sends us. */
            handle_ws_message(data->data_ptr, data->data_len);
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "Signaling WebSocket error");
            mark_ws_disconnected();
            break;
        default:
            break;
    }
}

esp_err_t webrtc_stream_init(const device_config_t *cfg, const char *device_id)
{
    /* Generated materials persist in internal memory until reset and are
     * reused for every subsequent handshake - generate exactly once. */
    if (esp_peer_pre_generate_cert() != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "pre_generate_cert failed");
        return ESP_FAIL;
    }

    esp_peer_default_cfg_t peer_default = {
        /* Longer ICE agent timeout: a browser offer over slow wifi plus a
         * TURN handshake can exceed the 100ms default. */
        .agent_recv_timeout = 500,
        .data_ch_cfg = {
            /* Stale frames are worthless after a stall - drop them fast
             * instead of the 5000ms default. */
            .cache_timeout = 1000,
            /* ~128KB outgoing cache holds several VGA frames mid-flight;
             * receive side is nearly unused (send-only device). */
            .send_cache_size = 128 * 1024,
            .recv_cache_size = 16 * 1024,
        },
        /* The device is always the answerer; without this, a disconnect
         * resets the role to controlling and the next session breaks. */
        .keep_role = true,
        .max_candidates = 16,
    };
    esp_peer_cfg_t peer_cfg = {
        .role = ESP_PEER_ROLE_CONTROLLED,
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .audio_dir = ESP_PEER_MEDIA_DIR_NONE,
        .video_dir = ESP_PEER_MEDIA_DIR_NONE,
        .enable_data_channel = true,
        /* The device creates "video_data" itself once SCTP is up; no
         * DCEP auto-channel. */
        .manual_ch_create = true,
        /* Sessions are driven by offers from the signaling socket; never
         * retry on our own. */
        .no_auto_reconnect = true,
        .on_state = on_peer_state,
        .on_msg = on_peer_msg,
        .on_data = on_peer_data,
        .extra_cfg = &peer_default,
        .extra_size = sizeof(peer_default),
    };

    int ret = esp_peer_open(&peer_cfg, esp_peer_get_default_impl(), &s_peer);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_open failed: %d", ret);
        s_peer = NULL;
        return ESP_FAIL;
    }

    strlcpy(s_device_id, device_id, sizeof(s_device_id));

    /* A ws:// or http:// prefix marks the local-development mode: the
     * device dials a plain-WebSocket server (`wrangler dev` on the LAN)
     * with no TLS. Anything else is production wss. The explicit :443 /
     * :80 matters: a hand-built ws transport has no default-port callback,
     * so a port-less URI would leave config->port = 0 and every connect
     * would dial port 0 (field-verified 2026-09-03: "Error connecting to
     * host ...:0" on the first ext_transport firmware). */
    bool plain_ws = (strncmp(cfg->backend_host, "ws://", 5) == 0 ||
                     strncmp(cfg->backend_host, "http://", 7) == 0);
    const char *scheme_prefix = strstr(cfg->backend_host, "://");
    const char *host_part = scheme_prefix ? scheme_prefix + 3 : cfg->backend_host;

    char uri[256];
    if (plain_ws) {
        /* Port-less local hosts default to :80 so config->port is never 0. */
        snprintf(uri, sizeof(uri), "ws://%s%s/signaling/%s", host_part,
                 strchr(host_part, ':') ? "" : ":80", device_id);
    } else {
        snprintf(uri, sizeof(uri), "wss://%s:443/signaling/%s",
                 cfg->backend_host, device_id);
    }

    /* The ws client keeps the header pointer, so it must outlive the client
     * - a static buffer rewritten only when init runs again (after the
     * previous client was destroyed on its failure path). */
    static char auth_header[24 + CONFIG_TOKEN_MAX_LEN + 1];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s\r\n",
             cfg->auth_token);

    /* Build the transport chain ourselves rather than letting the client
     * assemble one. Production (wss): pin the address family to IPv4 -
     * Cloudflare's front publishes AAAA records for the backend host, and
     * with lwIP caching only the first DNS answer the device can end up
     * holding just an IPv6 address, after which every connect dies on a
     * network whose IPv6 is not actually usable (field-verified 2026-09-03:
     * 10 s connect timeout loop while IPv4 clients on the same network
     * worked fine). CONFIG_LWIP_IPV6 must stay enabled because esp_peer
     * compiles against IPv6 socket types, so the IPv4-only rule is applied
     * here instead - getaddrinfo() then only ever returns an A record for
     * this host. Local development (ws://): plain TCP, no TLS at all - the
     * desktop dev server has no certificate. */
    esp_transport_handle_t parent_transport = NULL;
    esp_transport_handle_t ws_transport = NULL;
    if (plain_ws) {
        parent_transport = esp_transport_tcp_init();
    } else {
        parent_transport = esp_transport_ssl_init();
        if (parent_transport) {
            /* Same cert policy the client config used before: verify the
             * Worker's certificate against the bundled CA store. */
            esp_transport_ssl_crt_bundle_attach(parent_transport, esp_crt_bundle_attach);
            esp_transport_ssl_set_addr_family(parent_transport, ESP_TLS_AF_INET);
        }
    }
    if (!parent_transport) {
        ESP_LOGE(TAG, "Failed to create signaling transport");
        esp_peer_close(s_peer);
        s_peer = NULL;
        return ESP_FAIL;
    }
    ws_transport = esp_transport_ws_init(parent_transport);
    if (!ws_transport) {
        ESP_LOGE(TAG, "Failed to create signaling WS transport");
        esp_transport_destroy(parent_transport);
        esp_peer_close(s_peer);
        s_peer = NULL;
        return ESP_FAIL;
    }
    s_ws_parent_transport = parent_transport;
    s_ws_transport = ws_transport;
    ESP_LOGI(TAG, "Signaling transport: %s",
             plain_ws ? "plain ws (local dev)" : "wss + IPv4 pin");

    esp_websocket_client_config_t websocket_cfg = {
        .uri = uri,
        /* Hand the client our own wss transport (cert bundle + IPv4 pin are
         * on it); the client must not build its own. */
        .ext_transport = ws_transport,
        /* The Worker authenticates devices by this header on the handshake;
         * the shared token never appears in the URI or any URL. */
        .headers = auth_header,
        /* The backend's registry holds the device slot; after a drop,
         * reconnect promptly so the device reappears online. */
        .reconnect_timeout_ms = 3000,
        /* Also reconnect after a CLEAN server close (the Worker's 90 s
         * idle kill or a redeploy) - the client's default here is to give
         * up permanently, which field-verified 2026-09-03 turned the
         * device into a zombie that pinged a dead connection forever. */
        .enable_close_reconnect = true,
        /* Same as the component default, set explicitly to silence the
         * "using default time out" warning at boot. */
        .network_timeout_ms = 10000,
        /* Signaling messages (offer/answer SDPs) are a few KB; the default
         * 1KiB buffer would churn for every SDP. */
        .buffer_size = 8 * 1024,
    };

    s_ws = esp_websocket_client_init(&websocket_cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        /* The ext_transport is only attached inside client_start, so a
         * failed init never saw our handles - destroy them here. */
        esp_transport_destroy(s_ws_transport);
        esp_transport_destroy(s_ws_parent_transport);
        s_ws_transport = NULL;
        s_ws_parent_transport = NULL;
        esp_peer_close(s_peer);
        s_peer = NULL;
        return ESP_FAIL;
    }

    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);

    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: 0x%x", err);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        esp_transport_destroy(s_ws_transport);
        esp_transport_destroy(s_ws_parent_transport);
        s_ws_transport = NULL;
        s_ws_parent_transport = NULL;
        esp_peer_close(s_peer);
        s_peer = NULL;
        return ESP_FAIL;
    }

    /* Now that all failure paths are behind us, bring the peer loop up. */
    if (xTaskCreatePinnedToCore(peer_task, "peer", PEER_TASK_STACK, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create peer task");
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        esp_transport_destroy(s_ws_transport);
        esp_transport_destroy(s_ws_parent_transport);
        s_ws_transport = NULL;
        s_ws_parent_transport = NULL;
        esp_peer_close(s_peer);
        s_peer = NULL;
        return ESP_FAIL;
    }

    if (!s_staging) {
        s_staging = heap_caps_malloc(STAGING_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_staging) {
            /* Non-fatal: the streaming task falls back to holding the
             * framebuffer for the whole send (the pre-staging behavior). */
            ESP_LOGW(TAG, "Staging allocation failed, streaming without staging");
        }
    }
    if (!s_chunk) {
        s_chunk = heap_caps_malloc(CHUNK_WIRE_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_chunk) {
            ESP_LOGE(TAG, "Chunk buffer allocation failed - no PSRAM headroom");
        }
    }

    ESP_LOGI(TAG, "WebRTC stream ready: %s", uri);
    return ESP_OK;
}

uint8_t *webrtc_stream_staging_buffer(void)
{
    return s_staging;
}

size_t webrtc_stream_staging_size(void)
{
    return STAGING_SIZE;
}

esp_err_t webrtc_stream_send_frame(uint8_t *buf, size_t len)
{
    if (!s_peer || !s_chunk) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Drop garbage JPEGs (post-stall EV-EOF-OVF/NO-SOI frames are missing
     * SOI/EOI markers) instead of feeding the browser a corrupt frame. */
    if (len < 4 || buf[0] != 0xFF || buf[1] != 0xD8 || buf[len - 2] != 0xFF || buf[len - 1] != 0xD9) {
        ESP_LOGW(TAG, "Dropping invalid JPEG (%u bytes)", (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }

    if (!s_dc_open) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *data = buf;
    size_t left = len;
    while (left > 0) {
        size_t n = left > CHUNK_PAYLOAD_SIZE ? CHUNK_PAYLOAD_SIZE : left;
        bool first = (data == buf);
        bool last = (n == left);

        s_chunk[0] = (first ? CHUNK_FLAG_START : 0) | (last ? CHUNK_FLAG_END : 0);
        s_chunk[1] = (n >> 24) & 0xff;
        s_chunk[2] = (n >> 16) & 0xff;
        s_chunk[3] = (n >> 8) & 0xff;
        s_chunk[4] = n & 0xff;
        memcpy(s_chunk + 5, data, n);

        esp_peer_data_frame_t frame = {
            .type = ESP_PEER_DATA_CHANNEL_DATA,
            .data = s_chunk,
            .size = (int)n + 5,
        };
        int ret = esp_peer_send_data(s_peer, &frame);
        if (ret == ESP_PEER_ERR_WOULD_BLOCK) {
            /* The data channel cache is full. Never block or retry-loop
             * here - that backs the pipeline up into the camera (the
             * faccab9 failure). Drop the rest of the frame; the browser
             * resyncs on the next Start flag. */
            ESP_LOGW(TAG, "Data channel full - dropping rest of frame (%u of %u bytes sent)",
                     (unsigned)(data - buf), (unsigned)len);
            return ESP_ERR_NO_MEM;
        }
        if (ret != ESP_PEER_ERR_NONE) {
            ESP_LOGE(TAG, "send_data failed: %d", ret);
            return ESP_FAIL;
        }

        data += n;
        left -= n;
    }

    return ESP_OK;
}
