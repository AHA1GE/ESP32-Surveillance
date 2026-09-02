# Cloudflare Serverless Revision Plan

Revise the platform from a self-hosted Go backend (Docker + embedded coturn-style
TURN) to Cloudflare serverless: Workers + Durable Objects + D1 for signaling,
UI, and presence; Cloudflare Realtime TURN/STUN for NAT traversal; the ESP32
itself as a LAN fallback. Status: **design sketch, pre-implementation**.

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
- **Auth**: constant `AUTH_TOKEN` secret. Device sockets: `Authorization:
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

### 2.3 TURN / STUN (Cloudflare Realtime TURN)

- Prerequisite: one TURN key per account (dashboard → Calls, or
  `POST /accounts/{id}/calls/turn_keys` via API). **Done 2026-09-02**: key
  "ESP32-Surveillance", uid `384eca229df5f03573623c75607f02f1`. Key ID + API
  token are stored as Worker secrets (`TURN_KEY_ID`, `TURN_KEY_API_TOKEN` via
  `wrangler secret put`) — the API token is a long-term secret and must never
  be committed or reach devices/browsers.
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
- Per-allocation limits (50–100 Mbps, 5–10 kpps) are far above one JPEG
  stream; cost is $0.05/GB outbound after the shared 1 TB/mo free tier —
  effectively $0 for personal use, since LAN viewing never touches TURN.

### 2.4 D1 `esp32-surveillance`

Created (`c154a353-0375-44c1-a592-a26025fdc39e`, verified via API). Currently
unbound — `wrangler.jsonc` declares the D1 binding and the first
`wrangler deploy` attaches it.

`devices(id TEXT PRIMARY KEY, online INTEGER, first_seen TEXT, last_seen TEXT)`.
`/api/devices` keeps the exact contract `[{id, online, lastSeen}]` sorted by id
(ui.go:88-90).

## 3. Firmware changes (`esp32-firmware/`)

### 3.1 Signaling socket

- [webrtc_stream.c:454-456](esp32-firmware/main/webrtc_stream.c#L454-L456):
  URI `ws://%s:%lu/signaling/%s` → `wss://%s/signaling/%s` (drop the port;
  `uri[256]` is big enough).
- TLS: set `.use_global_ca_store = true` in the ws client config + one
  `esp_tls_set_global_ca_store(esp_crt_bundle_attach)` at boot. The full cert
  bundle is already compiled in (used by ota.c today) — no sdkconfig change.
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

### 3.3 LAN fallback (new — replaces the local backend)

When the signaling socket stays disconnected for ~60 s, the device starts a
**LAN JPEG server** instead of sitting dark:

- httpd (the portal code already ships one) serving `/` as
  `multipart/x-mixed-replace` MJPEG from the same frame producer the stream
  uses (`esp_camera_fb_get`, [camera.c:61](esp32-firmware/main/camera.c#L61) —
  same pattern as Espressif's `local_jpeg_stream`); single viewer, latest frame
  only, JPEG staged in the existing PSRAM buffer.
- mDNS `esp32cam-XXXXXX.local` (`esp_mdns`) so the browser can find it without
  knowing the IP.
- LED: solid = cloud connected (unchanged); **slow blink = LAN mode**; fast
  flash = error (unchanged). WS auto-reconnect (3 s) keeps trying; on
  CONNECTED the LAN server stops.

### 3.4 Firmware CI

[firmware.yml](.github/workflows/firmware.yml) is unchanged — the build
produces the new `.bin` automatically on push. No changes to `ota.c` needed for
this revision (manual flash, as today).

## 4. Repo layout and CI

- New `cloudflare/` — wrangler project:
  - `src/index.ts` (fetch handler, auth, routes), `src/devicehub.ts` (DO),
    `src/turn.ts` (cred minting + normalization), `assets/` (devices.html,
    view.html adapted).
  - `wrangler.jsonc`: custom domain `espcam.dofor.fun` (already attached to the
    worker), DO binding `DeviceHub` (hibernation), D1 binding, static assets,
    compatibility date ≥ DO hibernation.
  - `schema.sql` for D1; `scripts/sim-device.mjs` — a Node WS client that
    impersonates a device (connect, receive offer, send canned answer/ice) to
    smoke-test the DO without hardware.
- New workflow `.github/workflows/cloudflare.yml`: type-check/lint on PR only.
  Deployment stays manual (`npx wrangler deploy`) per the existing workflow
  split (Harry pushes/deploys; I commit). Secrets: `AUTH_TOKEN`,
  `TURN_KEY_ID`, `TURN_KEY_API_TOKEN` set via `wrangler secret`.
- `backend/` is **left untouched** until the Cloudflare path has survived a
  burn-in period, then deleted in a follow-up commit (with README/ROADMAP
  updates).

## 5. Execution phases

**Phase 0 — prerequisites: DONE (2026-09-02, verified via Cloudflare API).**

- TURN key "ESP32-Surveillance" (`384eca229df5f03573623c75607f02f1`).
- Worker `esp32-surveillance` (dashboard-created).
- Custom domain `espcam.dofor.fun` → that worker (zone dofor.fun, cert
  active).
- D1 `esp32-surveillance` (`c154a353-0375-44c1-a592-a26025fdc39e`) — unbound
  until the first `wrangler deploy` attaches the binding from wrangler.jsonc.
- Secrets: set by Harry. Cloudflare secrets are write-only, so names/values
  can't be verified via API — the code will expect `AUTH_TOKEN`,
  `TURN_KEY_ID`, `TURN_KEY_API_TOKEN`; rename/add in the dashboard if the
  existing secret differs.

**Phase 1 — Cloudflare side:** wrangler skeleton; Worker routes + auth + static
pages; `DeviceHub` with full protocol parity; D1 presence; TURN minting;
sim-device smoke test + `wrangler dev` against the real `view.html` in a
browser (mock device via script) and a real firmware device pointed at a
preview worker.

**Phase 2 — Firmware:** §3 changes; CI build; flash one camera; one-time portal
reconfiguration; verify: cloud live view, LAN-direct pair, forced relay (block
candidate paths or use cell network), and LAN fallback (kill the router's WAN
or point the host at a dead name → slow blink → MJPEG over mDNS).

**Phase 3 — Cutover:** flash remaining cameras; after a burn-in week, delete
`backend/` + docker bits, update README.md/ROADMAP.md, and mark this doc
implemented.

## 6. Verification checklist

1. `npx wrangler dev` + sim-device: viewer page gets `ice_servers`, offer
   reaches sim device with `stun`/`turn` attached, answer/ice round-trip,
   second viewer gets `busy` + 1008, offline device gets `device offline`,
   never-seen ID gets 404.
2. Real device: LED solid on connect; `/api/devices` shows online; list page
   renders; kill viewer tab → device gets `viewer_gone` (log), PC torn down.
3. Relay path: from a different network (or cell), confirm via the viewer's
   existing `getStats()` badge that the selected pair is a `relay` candidate
   pair; frames flow through CF TURN.
4. LAN fallback: sever WAN → LED slow blinks → `http://esp32cam-XXXXXX.local`
   streams JPEG; restore WAN → solid LED, LAN server stops.
5. Auth: WS without header/cookie → 401; wrong token → 401; browser
   login-with-token flow works and persists.
6. Firmware CI passes on push (existing workflow), Cloudflare PR workflow
   passes.

## 7. Risks and gotchas

- **CF TURN username length** — resolved by `ICE_CRED_MAX` → 128 (§3.1);
  verify against a real minted credential in Phase 1, before any firmware
  change.
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
