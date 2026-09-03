# Cloudflare Serverless Revision Plan

Revise the platform from a self-hosted Go backend (Docker + embedded coturn-style
TURN) to Cloudflare serverless: Workers + Durable Objects + D1 for signaling,
UI, and presence; Cloudflare Realtime TURN/STUN for NAT traversal; the ESP32
itself as a LAN fallback. Status: **Phase 1 (Cloudflare side) implemented and
smoke-tested against `wrangler dev`; Phase 2 (firmware) implemented in code
(2026-09-03) — pending push → CI build → flash → on-device verification.**

## Context

The current backend (`backend/`) requires a public IP, published UDP/TCP port
3478, and a Docker host — real infrastructure for what is otherwise a personal
camera system. Cloudflare now ships a managed TURN service
($0.05/GB after a shared 1 TB/mo free tier, UDP/TCP/TLS on anycast) plus the
serverless primitives (Workers, Durable Objects, D1) that can replace the Go
signaling hub entirely. This removes the VPS/docker/port-forwarding burden
while keeping the media path peer-to-peer: LAN first (host candidates), then
Cloudflare TURN as relay fallback.

Locked decisions (from Harry, 2026-09-02):

1. **Reachability**: cameras and viewing browser live outside mainland China —
   Cloudflare edge + `turn.cloudflare.com` are usable (the Cloudflare network
   explicitly excludes China).
2. **No local backend**: the Go backend is replaced, not mirrored. When the
   device cannot reach the Worker, the **ESP32 itself starts an HTTP server on
   the LAN** and serves live JPEG in real time — offline viewing survives.
3. **Auth**: shared bearer token. Devices send it as an `Authorization` header
   on the signaling socket; the browser enters it once and gets an HttpOnly
   cookie (browsers cannot set WS headers).
4. The per-minute HTTP heartbeat from the original idea is **dropped**:
   presence is derived from the signaling WebSocket itself (instant), plus a
   new 30 s app-level ping from the device for liveness. The device already
   holds a WS for signaling; a separate HTTP hook would add flash/CPU cost for
   worse (60 s-stale) data.

## Target architecture

```
Browser (anywhere)                    ESP32-CAM
    │ HTTPS + WSS                          │ WSS (TLS, cert bundle already compiled in)
    ▼                                      ▼
Cloudflare Worker  espcam.dofor.fun  (worker "esp32-surveillance")
    ├─ GET  /                      device list page (static asset + 5s poll)
    ├─ GET  /view/{id}             viewer page (adapted view.html)
    ├─ POST /api/login             token → HttpOnly cookie
    ├─ GET  /api/devices           JSON from D1: {id, online, lastSeen}
    ├─ WS   /signaling/{id}        ──┐ both proxied into one DO per deviceID
    └─ WS   /view-signaling/{id}   ──┘ DeviceHub (WebSocket hibernation)
           │                          │ mints CF TURN creds per session
           ▼                          ▼
        D1 `esp32-surveillance`      Cloudflare TURN  turn.cloudflare.com (UDP/TCP/TLS)
        devices(id, online, ...)   Cloudflare STUN  stun.cloudflare.com:3478

Media: device → browser WebRTC DataChannel "video_data" (unchanged wire format).
Host candidates connect LAN-direct; srflx via CF STUN; relay via CF TURN.
ICE priority order makes LAN-first emergent — no fallback state machine
(same principle as today, WEBRTC_PLAN.md "STUN-before-TURN is emergent").
```

The Go backend's state maps cleanly: the per-device `Device` struct
(conn + viewer + lastSeen, registry.go) becomes a Durable Object keyed by
deviceID — the DO gives the mutex and the viewer slot for free. The in-memory
registry (list wiped on restart) is replaced by D1, so the device list now
**survives deploys** (a strict improvement).

## 1. Protocol contract (preserved byte-for-byte)

The `SignalMsg` JSON envelope is the compatibility boundary. The Worker/DO must
emit exactly these messages; the firmware then needs **no signaling changes**.

Server → viewer:

| type | fields | notes |
|---|---|---|
| `ice_servers` | `stun[]`, `turn{urls,username,credential}` | first message, once per session |
| `answer` | `sdp` | |
| `ice` | `candidate` | |
| `peer_gone` | `reason:"device disconnected"` | then close 1000 |
| `error` | `reason:"device offline"` / `"busy: another viewer is already watching"` | close 1000 / 1008 |

Viewer → server: `offer{sdp}`, `ice{candidate}`, `ping` (every 20 s, swallowed).

Server → device:

| type | fields | notes |
|---|---|---|
| `offer` | `sdp` **+ `stun[]` + `turn{}`** | ICE servers ride the offer; device needs them before answering |
| `ice` | `candidate` | |
| `viewer_gone` | — | device tears down PeerConnection, frees heap |

Device → server: `answer{sdp}`, `ice{candidate}`, plus **new** `ping` every
30 s (see §3.3). Unknown types are ignored by all peers, so this grows forward.

Semantics to preserve in the DO: one-viewer-per-device (second viewer gets
`busy` + 1008), viewer evicted when the device disconnects or reconnects over
the slot, `viewer_gone` on viewer leave, viewer idle eviction at 60 s (rely on
the browser's existing 20 s pings — Workers has no read deadlines, so the DO
tracks last-activity per viewer and checks it on a 30 s alarm). Device IDs:
`^[a-zA-Z0-9-]{1,64}$`, MAC-derived `esp32cam-XXXXXX`, reused unchanged.

## 2. Cloudflare side (new `cloudflare/` wrangler project)

### 2.1 Worker (`esp32cam`)

- Routes above; pages are plain static assets (no CDN, matching today's
  offline-first templates). `devices.html`/`view.html` carry over with the same
  card markup, 5 s poll, and the existing canvas/datachannel JS.
- **Auth**: `SHARED_AUTH_TOKEN` secret. Device sockets: `Authorization:
  Bearer <token>` header. Browser: `POST /api/login {token}` sets
  `esp32cam_session` cookie (HttpOnly, Secure, SameSite=Lax, value = token);
  every page fetch and the WS upgrade (same-origin, cookies sent
  automatically) checks it; 401 responses make the page show a token prompt.
  Optionally tighten later with Cloudflare Access (ROADMAP.md already flags
  auth as required before public exposure — this migration is that exposure).
- WS routing: on `Upgrade: websocket` create a `WebSocketPair`, forward to the
  DO via `stub.fetch(url, {headers: {Upgrade: 'websocket'}})`, return the
  client end with 101 (hibernation pattern). Never-seen device ID → 404 from a
  D1 lookup before proxying (matches today's 404).

### 2.2 Durable Object `DeviceHub` (one per deviceID, `idFromName`)

- `hibernationWebSocket: true`; two tagged sockets: `device` (from
  `/signaling/{id}`) and `viewer` (from `/view-signaling/{id}`).
- `webSocketMessage` implements the relay table above plus: viewer `ping`
  refreshes last-activity; device `ping` refreshes device-activity; `offer`
  from viewer is forwarded with freshly minted `stun`/`turn` attached.
- `webSocketClose`/error → run the detach semantics of hub.go:97-108 /
  ReleaseViewer (evict viewer, `viewer_gone` to device, D1 update).
- **Device liveness**: alarm every 30 s; if device socket idle > 90 s, close it
  — the firmware's 3 s auto-reconnect then re-establishes (fixes the
  half-open-socket gap the Go hub left to TCP).
- **D1 writes** on device connect/disconnect: `online`, `last_seen` (RFC3339
  UTC). The DO's alarm also refreshes `last_seen` while connected, so the
  list's "last seen" stays current without an HTTP heartbeat.
- TURN creds: cached in the DO per session; re-mint only when a new viewer
  claims the slot.
- **workerd close-delivery quirk**: closes issued from `fetch`/`waitUntil`/
  `alarm`/`webSocketMessage` contexts on a hibernatable DO socket are *not*
  sent immediately — workerd flushes them ~10 s later (empirically pinned down
  with a scratch probe project, 2026-09-02; closes issued from the socket's own
  `webSocketClose` handler do deliver immediately). Codes and reasons arrive
  intact, so wire parity holds; only the close-frame latency shifts. Mitigation:
  terminal JSON (`error`/`peer_gone`) is sent first and arrives instantly, and
  well-behaved clients (the browser view page, the firmware) close on receiving
  it. The smoke test waits 15 s for server closes. See §7.

### 2.3 TURN / STUN (Cloudflare Realtime TURN)

- Prerequisite: one TURN key per account (dashboard → Calls, or
  `POST /accounts/{id}/calls/turn_keys` via API). **Done 2026-09-02**: key
  "ESP32-Surveillance", uid `384eca229df5f03573623c75607f02f1`. Key ID + API
  token are stored as Worker secrets (`TURN_SERVER_ID`, `TURN_SERVER_TOKEN`,
  set 2026-09-02) — the API token is a long-term secret and must never be
  committed or reach devices/browsers.
- Per viewing session the DO calls
  `POST https://rtc.live.cloudflare.com/v1/turn/keys/{id}/credentials/generate-ice-servers`
  (Bearer token, `{"ttl": 7200}` — 2 h, under the 48 h max, matching today's
  `TURN_CRED_HOURS`) and normalizes the response into the protocol shape:

  ```json
  "stun": ["stun:stun.cloudflare.com:3478"],
  "turn": {
    "urls": [
      "turn:turn.cloudflare.com:3478?transport=udp",
      "turn:turn.cloudflare.com:3478?transport=tcp",
      "turns:turn.cloudflare.com:5349?transport=tcp"
    ],
    "username": "<CF-generated>",
    "credential": "<CF-generated>"
  }
  ```

  UDP first (the firmware picks the `transport=udp` entry). The browser
  benefits from TCP/TLS fallbacks; the firmware uses only UDP by design.
  Skip the alternate ports 53/80/443 from the CF response — not useful here.
  **Verified 2026-09-02**: a real minted username is exactly 128 hex chars
  (the §3.1 `ICE_CRED_MAX` → 128 bump is mandatory), and the response contains
  the alternate ports as expected.
- Per-allocation limits (50–100 Mbps, 5–10 kpps) are far above one JPEG
  stream; cost is $0.05/GB outbound after the shared 1 TB/mo free tier —
  effectively $0 for personal use, since LAN viewing never touches TURN.

### 2.4 D1 `esp32-surveillance`

Created (`c154a353-0375-44c1-a592-a26025fdc39e`). Bound to the worker as
**`esp32_surveillance_db`** (underscores — a valid JS identifier, unlike the
dashboard's dashed name; `wrangler.jsonc` declares the same binding, so
deploys reuse the existing database instead of creating a duplicate).

Schema is initialized **in code** on first access (the ztmdl
room-reserve-system pattern): a `_meta` table records `schema_version`, and
`CREATE TABLE IF NOT EXISTS devices(id TEXT PRIMARY KEY, online INTEGER,
first_seen TEXT, last_seen TEXT)` runs when the version is missing — no
manual `wrangler d1 execute` step. `schema.sql` stays in the repo as the
authoritative reference. `/api/devices` keeps the exact contract
`[{id, online, lastSeen}]` sorted by id (ui.go:88-90).

## 3. Firmware changes (`esp32-firmware/`)

### 3.1 Signaling socket

- [webrtc_stream.c:454-456](esp32-firmware/main/webrtc_stream.c#L454-L456):
  URI `ws://%s:%lu/signaling/%s` → `wss://%s/signaling/%s` (drop the port;
  `uri[256]` is big enough).
- TLS: per-client `.crt_bundle_attach = esp_crt_bundle_attach` in the ws
  client config (implemented 2026-09-03; deviation from the global-CA-store
  wording above — the per-client hook matches ota.c and avoids a boot-time
  global store the portal never needs). The full cert bundle is already
  compiled in (used by ota.c today) — no sdkconfig change.
- Auth header: `websocket_cfg.headers = "Authorization: Bearer <token>\r\n"`
  (esp_websocket_client supports custom headers).
- App-level ping: send `{"type":"ping"}` every 30 s (counter in the existing
  10 ms peer-loop task). The library already auto-answers Cloudflare's WS
  control pings, which is what the edge requires.
- Bump `ICE_CRED_MAX` 64 → 128 ([webrtc_stream.c:44](esp32-firmware/main/webrtc_stream.c#L44)):
  Cloudflare TURN usernames are long and the current 64-byte `snprintf` would
  truncate them into broken credentials.

### 3.2 Config (v3 — one forced re-dooring)

- [config_store.h](esp32-firmware/main/config_store.h): `device_config_t` v3 —
  keep `wifi_ssid`, `wifi_password`, `backend_host` (now the Worker hostname,
  i.e. `espcam.dofor.fun`; the existing `[A-Za-z0-9.-]` charset already fits);
  add `auth_token[64]`; drop `backend_port` and the never-referenced
  `turn_server`.
- Bump `CONFIG_STORE_VERSION` → 3. This invalidates stored configs so every
  device opens the portal once after flashing: re-enter SSID/password + new
  host + token. **Expected and accepted** (no config migration exists by
  design, WEBRTC_PLAN.md:152-155).
- Portal ([web_portal.c](esp32-firmware/main/web_portal.c)): replace the port
  and TURN URL fields with the token field; same validation style.
- **Implemented 2026-09-03**: token validated non-empty + printable ASCII
  (0x21–0x7e) in `config_validate`; portal field is
  `type="password" maxlength="64"`, prefilled and HTML-escaped like the rest.

### 3.3 LAN fallback (new — replaces the local backend)

When the signaling socket stays disconnected for ~60 s, the device starts a
**LAN JPEG server** instead of sitting dark:

- httpd (the portal code already ships one) serving `/` as
  `multipart/x-mixed-replace` MJPEG from the same frame producer the stream
  uses (`esp_camera_fb_get`, [camera.c:61](esp32-firmware/main/camera.c#L61) —
  same pattern as Espressif's `local_jpeg_stream`); single viewer, latest frame
  only, JPEG staged in the existing PSRAM buffer.
- mDNS `esp32cam-XXXXXX.local` so the browser can find it without knowing the
  IP — the `espressif/mdns` managed component (moved out of IDF core in v5.4+;
  un-prefixed `mdns_*` API, added to idf_component.yml + CMake PRIV_REQUIRES).
- LED: solid = cloud connected (unchanged); **slow blink = LAN mode**; fast
  flash = error (unchanged). WS auto-reconnect (3 s) keeps trying; on
  CONNECTED the LAN server stops.
- **Implemented 2026-09-03** as new [lan_stream.c](esp32-firmware/main/lan_stream.c):
  port-80 httpd, handler grabs the latest frame straight from the camera (the
  fb already lives in the PSRAM staging buffer — no copy), single viewer by
  construction (blocking handler on httpd's single server task), a
  `s_stopping` flag set before `httpd_stop` so a dead camera can't wedge the
  shutdown. mDNS failure is warn-only — the stream still serves by raw IP.
  Fallback triggers at 60 s disconnected, counters live in the existing
  peer-loop task; LED slow-blink via the queue-driven LED task (never LEDC —
  the camera owns the timer).

### 3.4 Firmware CI

[firmware.yml](.github/workflows/firmware.yml) is unchanged — the build
produces the new `.bin` automatically on push. No changes to `ota.c` needed for
this revision (manual flash, as today).

## 4. Repo layout and CI

- New `cloudflare/` — wrangler project:
  - `src/index.ts` (fetch handler, auth, routes), `src/devicehub.ts` (DO),
    `src/db.ts` (in-code schema init), `src/turn.ts` (cred minting +
    normalization), `assets/` (devices.html, view.html adapted).
  - `wrangler.jsonc`: custom domain `espcam.dofor.fun` (already attached to the
    worker), DO binding `DEVICE_HUB` (hibernation) with `DeviceHub` exported
    from the entrypoint, D1 binding `esp32_surveillance_db`, static assets
    (`run_worker_first` for `/`, `/view/*`, `/api/*`), compatibility date
    2026-09-02.
  - `schema.sql` (reference); `scripts/sim-device.mjs` — a Node WS client that
    impersonates a device (connect, receive offer, send canned answer/ice);
    `scripts/smoke-test.mjs` — one-shot e2e suite (auth gates, login flow, D1
    presence, full negotiation, busy/offline/supersede) with unique per-run
    device IDs so stale local-D1 rows can't trip assertions.
  - `.dev.vars.example` for local `SHARED_AUTH_TOKEN`; `.dev.vars` is
    gitignored. Locally, without `TURN_SERVER_TOKEN`, minting degrades to
    stun-only (smoke test reports `turn=stun-only`).
- New workflow `.github/workflows/cloudflare.yml`: type-check/lint on PR only.
  Deployment stays manual (`npx wrangler deploy`) per the existing workflow
  split (Harry pushes/deploys; I commit). Secrets: `SHARED_AUTH_TOKEN`,
  `TURN_SERVER_ID`, `TURN_SERVER_TOKEN` set via `wrangler secret`.
- `backend/` is **left untouched** until the Cloudflare path has survived a
  burn-in period, then deleted in a follow-up commit (with README/ROADMAP
  updates).

## 5. Execution phases

**Phase 0 — prerequisites: DONE (2026-09-02, verified via Cloudflare API).**

- TURN key "ESP32-Surveillance" (`384eca229df5f03573623c75607f02f1`).
- Worker `esp32-surveillance` (dashboard-created).
- Custom domain `espcam.dofor.fun` → that worker (zone dofor.fun, cert
  active).
- D1 `esp32-surveillance` (`c154a353-0375-44c1-a592-a26025fdc39e`) — created
  and bound to the worker (verified via API). The dashboard-created binding
  was named `esp32-surveillance-db`; the wrangler project redeclares it as
  `esp32_surveillance_db` (valid JS identifier), so the first `wrangler
  deploy` replaces the binding name but keeps the same database_id.
- Secrets/vars: set by Harry and verified via API (the script bindings list
  exposes names, and plain-text values — only `secret_text` values are
  hidden): `SHARED_AUTH_TOKEN` (secret_text), `TURN_SERVER_ID` (plain_text,
  value matches the TURN key uid), `TURN_SERVER_TOKEN` (secret_text).

**Phase 1 — Cloudflare side: DONE (2026-09-02).** wrangler skeleton; Worker
routes + auth + static pages; `DeviceHub` with full protocol parity; D1
presence (in-code schema init); TURN minting + normalization. The smoke suite
passes end-to-end against `wrangler dev` (30/30 checks: auth gates, login
flow, D1 online presence, ice_servers → offer → answer → ice relay, busy
1008, offline error, peer_gone + close 1000 on disconnect, supersede close
1000 + viewer eviction on reconnect). Known quirk: server closes from
waitUntil/alarm contexts arrive ~10 s late (§2.2, §7) — terminal JSON
messages are unaffected.

**Deployed 2026-09-02 via Workers Builds (dashboard Git integration):** every
push to `main` builds `cloudflare/` and deploys the worker automatically
(latest deployment 11:11Z, source wrangler). Verified via API + live checks:
`DEVICE_HUB` DO namespace created, D1 binding renamed to
`esp32_surveillance_db` (same database_id, data preserved), `ASSETS` bound,
compat date 2026-09-02, all three secrets preserved; live auth gates on
espcam.dofor.fun all correct (401 login page, 401 JSON, 401 WS).

**Live smoke test passed 30/30 against espcam.dofor.fun (2026-09-02,
real token, `turn=minted`)** — TURN credential minting works in production.
Production D1 was empty before the run (no real device had connected to the
new worker yet); the run left one offline `esp32cam-SMOKE-*` presence row.
Remaining: a browser test with a real firmware device (Phase 2).

**Phase 2 — Firmware:** §3 code implemented and committed (2026-09-03; files:
webrtc_stream.c, config_store.h/.c, web_portal.c, lan_stream.c/.h [new],
led.h/.c, app_main.c, main/CMakeLists.txt, main/idf_component.yml, and — from
the first on-device test the same day — sdkconfig.defaults + webrtc_stream.c
for the IPv6/AAAA reachability fix and the LED/timer fixes, see §7).
Remaining
(Harry): push → firmware CI builds (release-v6.0) → flash one camera → one-time
portal reconfiguration (v3 wipes stored config: re-enter SSID/password + host
`espcam.dofor.fun` + token) → verify: cloud live view, LAN-direct pair, forced
relay (block candidate paths or use cell network), and LAN fallback (kill the
router's WAN or point the host at a dead name → slow blink → MJPEG over mDNS).

**Phase 3 — Cutover:** flash remaining cameras; after a burn-in week, delete
`backend/` + docker bits, update README.md/ROADMAP.md, and mark this doc
implemented.

## 6. Verification checklist

1. ✅ `scripts/smoke-test.mjs` (2026-09-02): 30/30 both against
   `wrangler dev` and live against https://espcam.dofor.fun with the real
   token — viewer gets `ice_servers` (`turn=minted` in production), offer
   reaches sim device with `stun`/`turn` attached, answer/ice round-trip,
   second viewer gets `busy` + close 1008, offline device gets
   `device offline`, never-seen ID gets 404, device disconnect →
   `peer_gone` + close 1000, reconnect → supersede close 1000 + viewer
   eviction.
2. Real device: LED solid on connect; `/api/devices` shows online; list page
   renders; kill viewer tab → device gets `viewer_gone` (log), PC torn down.
3. Relay path: from a different network (or cell), confirm via the viewer's
   existing `getStats()` badge that the selected pair is a `relay` candidate
   pair; frames flow through CF TURN.
4. LAN fallback: sever WAN → LED slow blinks → `http://esp32cam-XXXXXX.local`
   streams JPEG; restore WAN → solid LED, LAN server stops.
5. Auth: WS without header/cookie → 401; wrong token → 401; browser
   login-with-token flow works and persists.
6. Firmware CI passes on push (existing workflow, release-v6.0 — pending the
   first Phase 2 push), Cloudflare PR workflow passes.

## 7. Risks and gotchas

- **Cloudflare AAAA records vs a half-usable IPv6 network (field-verified
  2026-09-03)** — the first on-device test looped: TLS connect to the Worker
  timed out every 10 s (errno 119, select() timeout) while IPv4 clients on the
  same network worked. Root cause: the device resolved the backend host to
  its AAAA record and the connect went over a v6 path that hangs instead of
  failing fast, with lwIP caching only one address per hostname so no v4
  fallback was available. Fix: the signaling WebSocket TLS transport is
  pinned to `ESP_TLS_AF_INET` in webrtc_stream.c (the client is handed our own
  `ext_transport`, built with `esp_transport_ssl_set_addr_family`), so
  getaddrinfo() only ever returns an A record for the backend host.
  `CONFIG_LWIP_IPV6` must stay enabled — esp_peer 1.5.4 uses
  `struct sockaddr_in6` without `LWIP_IPV6` guards and IDF v6's esp_netif
  likewise, so an IPv4-only lwIP does not compile; esp_peer itself already
  runs with `ipv6_support=false` (its default), so the WS connect was the
  only IPv6-exposed path. Also fixed on the same
  pass: the ws error handler repainted FAST_FLASH over the LAN mode's
  SLOW_BLINK on every failed reconnect (LAN mode now owns the LED via one
  `led_update()`), and the 30 s ping / 60 s fallback timers moved from
  peer-loop tick counting to wall clock so a stretched `esp_peer_main_loop`
  can't delay them.
- **Server-initiated WS close zombified the device (field-verified
  2026-09-03)** — after the Worker closed the signaling socket (its 90 s idle
  kill during a WiFi outage, or a redeploy), the device pinged a dead
  connection every 30 s for 30+ minutes: the client emitted
  `WEBSOCKET_EVENT_CLOSED` (an event the handler didn't cover) and, with the
  default `enable_close_reconnect=false`, stops reconnecting permanently.
  The app's `s_ws_connected` stayed true, so the 60 s offline counter never
  ran and the LAN fallback never engaged (port 80 refused). Fixes in
  webrtc_stream.c: the CLOSED event is now handled like DISCONNECTED,
  `enable_close_reconnect=true` makes the client reconnect after a clean
  close, and a failed app ping cross-checks `esp_websocket_client_is_connected()`
  and self-heals a stale flag within one 30 s cycle.
- **CF TURN username length** — resolved by `ICE_CRED_MAX` → 128 (§3.1);
  verified against a real minted credential (exactly 128 hex chars,
  2026-09-02).
- **workerd close-delivery quirk (pinned down empirically, 2026-09-02)** — on
  hibernatable DO sockets, `ws.close()` issued from `fetch`, `waitUntil`,
  `alarm`, or `webSocketMessage` contexts is not sent immediately: workerd
  flushes it ~10 s later (measured 9.5 s in a scratch probe; closes issued
  from the socket's own `webSocketClose` handler deliver at once; re-close
  attempts on an already-closing socket are no-ops; plain-Worker sockets are
  unaffected). Codes/reasons arrive intact, so the wire protocol is preserved
  — only the close frame's timing shifts. Consequences: busy/offline
  rejections and supersede/eviction closes land ~10 s after the terminal
  JSON (`error`/`peer_gone`), which arrives instantly; idle kills (60/90 s)
  land ~10 s after the alarm fires. The browser and firmware already treat
  the terminal JSON as the signal and close their ends. Do **not** "fix"
  this by closing from a different context without re-probing — message-
  context closes never flushed at all in probing.
- **Browsers can't set WS headers** — cookie flow (§2.1); token never appears
  in URLs (no query-param leak into logs).
- **Workers can't send WS control pings to the device** — app-level `ping`
  both directions instead (§3.1); the edge's own 20 s ping/pong is handled by
  the ws library automatically.
- **DO hibernation timing** — D1 writes happen inside message handlers, not
  after-the-fact, so hibernation (10 s after last event) never loses state.
- **LAN fallback RAM** — MJPEG reuses the existing PSRAM staging buffer;
  single viewer; keep the httpd stack small.
- **Config v3 wipes stored WiFi** — every camera needs one manual
  reconfiguration through the portal after flashing; budget for it.
- **Outage trade-off accepted** — internet down = LAN JPEG fallback only (no
  WebRTC viewer, no TURN). This is the agreed cost of deleting the local
  backend.
- `sig.ahaigege.com` is taken by the unrelated `dyn-sig` worker — do not reuse
  that hostname.

## 8. Cost summary

TURN: free ≤ 1 TB/mo, then $0.05/GB outbound. Workers/DO/D1: comfortably
inside free tiers at this scale (a few devices, occasional viewing, WS
hibernation keeps DO billing near zero). Expected monthly cost: **$0**.
