# WebRTC DataChannel rearchitecture — real-time only

Design record for moving live viewing from the ffmpeg/HLS backend pipeline to
a direct device-to-browser WebRTC DataChannel. **The system becomes real-time
only: no recording, no playback, no SD card, no archive.** Not yet
implemented — this is the plan to work from.

## Context

Today the device streams JPEG frames over a plain WebSocket to a Go backend,
which runs one `ffmpeg` per device to transcode MJPEG into HLS + archive MP4
on local disk ([backend/internal/pipeline/pipeline.go](backend/internal/pipeline/pipeline.go)).

The last five commits on `main` are all fixes for that pipeline fighting
itself: PSRAM bus contention between camera DMA and WebSocket buffers
(`201204f`), WebSocket message fragmentation killing the connection
(`eb8ee61`), corrupt JPEGs after stalls and slow reconnect (`faccab9`), and a
wall-clock-timestamp workaround because `image2pipe` assumes a fixed 25fps
while the camera delivers a variable ~5-10fps (`fbc2ed3`). That last one is
the tell: most of the pain comes from forcing a variable-rate JPEG stream
into a fixed-rate video container it was never suited to.

This change removes the video-container model entirely. The device keeps
doing only capture + send (the OV2640 already emits JPEG from its own ISP —
there has never been a software encode on-device). Frames travel to the
browser over a WebRTC SCTP DataChannel as chunked JPEG blobs, and the browser
paints them to a `<canvas>`. No H.264, no HLS, no MSE, no ffmpeg. The backend
shrinks to signaling + STUN/TURN + a static page, holding no durable state.

**Recording is dropped everywhere, not just on the device.** No SD card, no
FATFS/SDMMC code, no archive MP4s on the backend, no playback endpoints, no
`auto_record` checkbox. A frame that isn't watched live is gone forever. If
viewing isn't live, it doesn't exist.

### Why no SD

The previous revision of this plan carried SD recording with an agreed escape
hatch ("if it proves unworkable, drop it"). This revision pulls the hatch up
front. Each argument below was already in the original plan as a *risk*;
taken together they outweigh the feature:

- **Brownout margin.** The board already trips on PC USB power — camera +
  WiFi TX peaks alone. SD writes spike current on exactly the moments the
  camera and radio are busy.
- **PSRAM bus.** SDMMC DMA and camera DMA share the PSRAM bus. The project's
  two worst bugs (`201204f`, `faccab9`) were both bus-contention failures,
  and DTLS + SCTP are about to join that bus. SD would be a third tenant.
- **Flash budget.** FATFS + sdmmc + vfs costs flash that the esp_peer stack
  may need (step 2 decides).
- **FAT is hostile to frame-rate writes** — metadata, not bandwidth, is the
  bottleneck. One file per frame was ruled out on directory-scan cost;
  AVI segments were the workaround, and segments are still fsync stalls and
  directory churn inside the capture loop.
- **No retained value.** Recordings were write-only (pull the card) and
  nobody watches them; the backend archive side was slated for deletion
  anyway.

The real-time-only system has exactly one hard problem (WebRTC on a classic
ESP32) instead of two.

### Decisions taken

| Question | Decision |
|---|---|
| Recording / playback | **None, anywhere** — real-time only |
| SD card | Dropped from scope entirely (no SDMMC, no FATFS) |
| Concurrent viewers per device | Exactly 1 |
| NAT traversal | STUN first, TURN relay only when direct fails |
| Existing ffmpeg/HLS/archive pipeline | Delete entirely |
| Flash budget | Grow OTA slots; fall back to **single OTA slot** if it won't fit |
| TURN deployment | Embed `pion/turn/v4` in the Go backend binary |

## Hardware status as of 2026-09-02

`ROADMAP.md` is half-updated: the confirmed items below are ticked, but the
~60s STA-timeout fallback is bundled inside the ticked portal item when it is
in fact unverified, and the Implemented section still describes the
HLS/archive system this plan deletes. Step 1 fixes the statuses; step 7
rewrites the Implemented section. Actually confirmed:

- Camera init succeeds with PSRAM on a real AI-Thinker ESP32-CAM, and frames
  reach the backend (evidenced by the EV-EOF-OVF / NO-SOI debugging behind
  `201204f` and `eb8ee61` — those are hardware observations, not theory).
- Config portal flow end to end: first flash → AP + form → save → reboot →
  streaming.
- GPIO33 LED patterns, and the portal form renders on a phone browser.

Still unverified: the ~60s STA-timeout fallback to the portal, two devices
running simultaneously, and the release/OTA path.

**Current stream behaviour, and it matters for this plan:** at `201204f` the
stream works **only when the board is standalone-powered — not when connected
to a PC over USB serial** — and the browser view still freezes intermittently.
The latest commit (`faccab9`, PSRAM staging + fast reconnect) has not been
tested on hardware at all.

The USB-vs-standalone split is the classic ESP32-CAM brownout signature: a PC
USB port often can't supply the current peaks of camera + WiFi TX. Research
flagged this as the usual root cause of a long list of "mysterious" failures
on this board — SD corruption reports among them, which is one more reason SD
is out of this plan. **Treat a known-good 5V supply with bulk capacitance
(470–1000µF) as a prerequisite, not a debugging step.**

## Key research finding

The one finding that changed the shape of this plan is worth stating up
front. (The previous revision's FAT-layout research — one-JPEG-per-file vs
AVI segments — is moot with SD gone.)

**Espressif has already built almost exactly this.**
`solutions/local_jpeg_stream` in
[esp-webrtc-solution](https://github.com/espressif/esp-webrtc-solution)
streams camera JPEGs over a WebRTC DataChannel to a browser. Use it as the
reference implementation rather than designing the wire format from scratch:

- Chunked at **10000 bytes** (`video_dc_chunked = true`,
  `video_dc_chunk_size = 10000`). A whole ~40KB JPEG in one
  `esp_peer_send_data()` would probably work, but Espressif deliberately does
  not do this — chunk.
- Wire format, quoted from `esp_webrtc.h`:
  `[1B flag][4B BE len][payload]`, `flag bit0=Start, bit1=End`.
- Channel: `ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_RETX`, `ordered = false`,
  `max_retransmit_count = 1`, label `"video_data"`. Partial reliability is
  right for live video — a late frame is worthless.
- Their `webrtc_test.html` shows the browser-side reassembly.

## Constraints discovered

- **Flash is the binding risk, and it is unproven.** Two 1536K OTA slots on a
  4MB chip; current image ~1.03MB. Estimated additional cost of esp_peer
  (ICE + DTLS + SCTP + libsrtp) is **~250–450KB, which is an estimate, not a
  published figure**. Espressif's own WebRTC reference designs use a **3.5MB
  single-factory partition with no OTA slots at all** — a loud signal. This is
  why step 2 below is a measurement spike.
- **DTLS is not currently enabled.** `sdkconfig.defaults` enables only the
  mbedTLS certificate bundle. esp_peer's README requires
  `CONFIG_MBEDTLS_SSL_PROTO_DTLS=y` and `CONFIG_MBEDTLS_SSL_DTLS_SRTP=y`.
- **Classic ESP32 is supported but untrodden.** `esp_peer/libs/esp32/libpeer_default.a`
  ships, so the target is real — but **all 14 shipped solutions target S3/P4**.
  Expect to hit issues nobody has hit.
- **Data-channel-only SDP is plausible but unproven.** `ESP_PEER_MEDIA_DIR_NONE`
  exists and the changelog implies media-less negotiation works, but no shipped
  example is data-channel-only. Smoke-test against a browser early.
- **PSRAM bus saturation is a documented, already-hit failure mode.**
  `camera.c` and `sdkconfig.defaults` both carry comments explaining that SVGA
  frames plus resident PSRAM WebSocket buffers caused EV-EOF-OVF camera
  overflows that killed the stream every ~12s. Adding the esp_peer stacks
  (DTLS + SCTP) on the same bus is the largest regression risk here.
- **Brownout is already observable on this board** (works standalone, fails on
  PC USB — see Hardware status). Camera + WiFi TX peaks alone trip it. Fix
  the supply before blaming code.
- **Config schema has no migration.** `config_store_load` does strict equality
  on both blob length and version, despite a comment claiming otherwise. New
  fields change `sizeof` and invalidate every stored config → device drops to
  the AP portal. Acceptable: there is no deployed fleet to strand.
- **`backend_host` validation forbids `:` `/` `_` `%`** — accepts only
  `[A-Za-z0-9.-]`. Signaling URLs and TURN credentials need their own validator.
- **Portal form limits.** `MAX_FORM_BODY` is 1024 and hard-rejects longer POSTs;
  `parse_form` already uses ~2.1KB of the httpd task's 4096-byte stack.
- **Project pinned to ESP-IDF v6** (`idf: ">=6.0"` in the manifest + a CMake
  guard; CI builds v6.0.2 — pinned at the patch level because Docker's `v6.0`
  tag is frozen at the initial release, see `espressif/esp-idf#15411`,
  target `esp32`) — esp_peer 1.5.4's prebuilt library is built against v6's
  log API. No `sdkconfig` is committed, so every new setting must go in
  `sdkconfig.defaults`. Every new `#include <>` needs its component in
  `PRIV_REQUIRES` — this exact omission broke CI before (`3acd4ac`).

## Plan

### 0. Branch

Create `webrtc-datachannel` off `main`. `main` stays on the WebSocket/HLS
build as a fallback — with the caveat that its current HEAD (`faccab9`) is
itself untested, which step 1 of Verification addresses.

### 1. Update ROADMAP.md first (small, do it immediately)

The "Needs hardware to verify" section has the three confirmed items ticked,
but the ~60s STA-timeout fallback is bundled inside the ticked portal item
and is in fact unverified — split it into its own unchecked item. Record the
two live findings — that the board is only stable standalone-powered, and
that the browser view still freezes intermittently at `201204f` with
`faccab9` untested. Leave the Implemented-section rewrite (HLS/archive →
real-time only) to the step 7 docs sweep.

This matters beyond bookkeeping: the plan leans on "main is a known-good
fallback," and the roadmap is where that claim is either true or not.

### 2. Flash measurement spike — before any implementation

Roughly an hour of work that de-risks everything downstream. Build a stub app
with `espressif/esp_peer` + the existing camera driver (no SD, no FATFS — that
whole dependency chain is gone from this plan), and run
`idf.py size-components` to get the **real** esp_peer contribution.

Then pick the partition layout from the measured number:

- **If the app fits comfortably under ~1900K**, grow both slots in
  [partitions.csv](esp32-firmware/partitions.csv) to `1920K`
  (`ota_0 @0x20000`, `ota_1 @0x200000`, ending 0x3E0000 and leaving 128KB
  slack — preferred over the theoretical 1984K maximum, which would leave
  exactly zero).
- **Otherwise**, drop to a single ~3.5MB app partition, matching Espressif's
  reference designs. This gives up OTA rollback: a bad update then needs a
  serial reflash.

Either way partition offsets move, so **every device needs a serial reflash
and cannot OTA across this change**. Free now (nothing deployed), very painful
later. Land it before any other firmware work so all later builds target the
final layout.

Size levers if it's marginal: `CONFIG_COMPILER_OPTIMIZATION_SIZE`, stripping
unused mbedTLS ciphersuites, and dropping the inert OTA path
([esp32-firmware/main/ota.c](esp32-firmware/main/ota.c) plus the cert bundle,
`esp_https_ota` and cJSON) which has never actually run.

**Measured (2026-09-02, IDF v5.4, esp_peer 1.5.4):** the spike measured
esp_peer's real flash cost — `libpeer_default.a` ≈ 50KB, the esp_peer wrapper
≈ 21KB, `esp_libsrtp` ≈ 27KB (≈ 100KB total) — and the full firmware links at
~1.2MB. Comfortably under ~1900K, so both OTA slots were grown to `1920K` in
[partitions.csv](esp32-firmware/partitions.csv). Two link-time findings from
the spike: (1) esp_peer's prebuilt is built against IDF v6's `esp_log()` API —
the project is therefore pinned to ESP-IDF v6 (`idf: ">=6.0"` + CMake guard),
matching the library's build environment instead of shimming; (2) X.509
creation for the DTLS self-signed cert is unconditional in IDF v5.x, but is a
real Kconfig option from v6 on — `CONFIG_MBEDTLS_X509_CREATE_C=y` is set in
`sdkconfig.defaults`.

### 3. Firmware — config and portal

Add to `device_config_t` in
[esp32-firmware/main/config_store.h](esp32-firmware/main/config_store.h):
signaling host/port, and TURN server URL. TURN *credentials* are deliberately
**not** stored — they arrive per-session over signaling (see step 5). These
exceed the 64-byte `reserved` block, so `sizeof` grows: bump
`CONFIG_STORE_VERSION` to 2 and accept that existing configs drop to the portal.

**Remove `auto_record`** — the field and the checkbox. With SD gone there is
nothing left for it to gate, and "reserved, future use" is now "never". The
version bump absorbs the struct change for free. `auto_flash` stays: it gates
the LED flashlight, which is a camera function, not a recording function.

- Write a new validator for URL-ish fields; **do not** reuse the `backend_host`
  charset rule, which rejects `:` and `/`.
- [esp32-firmware/main/web_portal.c](esp32-firmware/main/web_portal.c): add the
  new fields to `FORM_TEMPLATE` and delete the `auto_record` checkbox in the
  same pass (args are positional — reorder the `snprintf` call to match), add
  `else if (strcmp(key_dec, ...))` branches in `parse_form`, add `*_esc[]`
  buffers in `send_form`, grow `page[4096]`, raise `MAX_FORM_BODY` above 1024,
  and raise `config.stack_size` in `web_portal_start()` (or make `val_dec`
  static) given `parse_form`'s existing ~2.1KB stack use.

### 4. Firmware — WebRTC DataChannel

New `main/webrtc_stream.c` / `.h` replacing
[esp32-firmware/main/ws_stream.c](esp32-firmware/main/ws_stream.c) as the
transport. Add to
[esp32-firmware/main/idf_component.yml](esp32-firmware/main/idf_component.yml):

```yaml
  espressif/esp_peer: "^1.5.4"
```

That is the **only** new managed dependency — `espressif/esp_libsrtp` comes in
automatically, and esp_peer has its own bundled ICE and SCTP (so
`libjuice`/`esp_usrsctp`/`sepfy/libpeer` are all unnecessary and must not be
added alongside). Add the DTLS options to `sdkconfig.defaults`.

- **Custom signaling impl** over the existing `esp_websocket_client`: the
  interface is only three function pointers (`start`/`send_msg`/`stop`) plus
  callbacks. Model on `apprtc_signal/signal_default.c`, which is also
  WebSocket-based. The `on_ice_info` callback is how the backend hands the
  device its per-session TURN credentials.
- **Device is the answerer**: it holds only the signaling socket while idle and
  builds the PeerConnection on demand when a viewer arrives, tearing it down on
  disconnect. With one viewer max this keeps DTLS/SCTP state off the heap the
  rest of the time.
- Data-channel-only: `audio_dir` and `video_dir` = `ESP_PEER_MEDIA_DIR_NONE`,
  `enable_data_channel = true`, `manual_ch_create = true`. Leave `rtp_cfg`
  zeroed. Call `esp_peer_create_data_channel()` only after
  `ESP_PEER_STATE_DATA_CHANNEL_CONNECTED`.
- Use `esp_peer_pre_generate_cert()` — DTLS handshake keygen is the slow part
  on classic ESP32 and this is what it exists for.
- Frame send: chunk at 10000 bytes using Espressif's
  `[1B flag][4B BE len][payload]` format. Keep the SOI/EOI validity check from
  `ws_stream_send_frame` — it drops the corrupt post-stall frames documented in
  `faccab9`.
- **Handle `ESP_PEER_ERR_WOULD_BLOCK`** by dropping the rest of the current
  frame and resyncing on the next capture. Do not block and do not retry-loop;
  backing up the pipeline is precisely the `faccab9` failure. Note `peer_demo`
  ignores this return value — don't copy it.
- Put `send_cache_size`/`recv_cache_size` in PSRAM; start ~128–200KB (Espressif
  uses 400KB, but on S3/P4). Lower `cache_timeout` to ~500–1000ms from the
  5000ms default so stale frames aren't sent after a stall.
- Pin `esp_peer_main_loop()` to one core, camera to the other.
- Keep the per-10s fps/heap telemetry in `streaming_task` — it's the main
  instrument for catching a PSRAM-contention regression.

Delete `ws_stream.c` once this works.

### 5. Backend — Go

Delete `internal/pipeline`, `internal/retention`,
[backend/internal/httpapi/archive.go](backend/internal/httpapi/archive.go),
[backend/internal/httpapi/live.go](backend/internal/httpapi/live.go), and the
vendored hls.js asset. Drop `ffmpeg` from
[backend/Dockerfile](backend/Dockerfile) and the ffmpeg/HLS/archive/retention
env vars from [backend/internal/config/config.go](backend/internal/config/config.go).

After this change the backend stores no frame data anywhere: no disk, no
archive, no playback endpoints. A device's history is just its presence in the
device list.

- `internal/registry` shrinks to an in-memory table of online devices and their
  signaling sockets — no `pipeline.Device`, no disk paths. Keep `ValidDeviceID`
  from [backend/internal/registry/validate.go](backend/internal/registry/validate.go)
  unchanged; still the right guard for IDs in URLs.
- [backend/internal/ingest/ws.go](backend/internal/ingest/ws.go) becomes
  `internal/signaling`: devices connect and stay connected, viewers connect per
  session, the server relays SDP and ICE candidates and enforces one viewer per
  device.
- **STUN before TURN — and it's free.** ICE already tries candidates in
  priority order (`host` → `srflx` → `relay`), so simply *listing* a STUN server
  ahead of TURN means the relay is only used when direct fails. No extra logic,
  no fallback state machine. Better still, `pion/turn` answers STUN binding
  requests on the same port it serves TURN, so **you get your own STUN server
  for free from the same binary** — no dependency on Google's public STUN and
  nothing leaked to a third party. Hand the device and browser an ICE server
  list of `[stun:<backend>:3478, turn:<backend>:3478 + session credentials]`.
  Adding a public STUN as a secondary entry is optional redundancy; it buys
  little once you're running your own and costs an external dependency.
- **TURN**: embed `github.com/pion/turn/v4` (the only new Go dependency beyond
  `coder/websocket`). `turn.NewServer` starts serving immediately — there's no
  separate `Listen()`. Use `turn.LongTermTURNRESTAuthHandler(secret, logger)`
  for time-limited credentials, minted per viewing session and delivered to both
  peers over signaling, so no long-lived secret sits in NVS or page source. That
  handler is coturn-compatible, so switching to a standalone coturn later needs
  no device-side change. Constrain the relay port range
  (`RelayAddressGeneratorPortRange`, ~5000 ports). Serve UDP **and** TCP 3478 —
  esp_peer v1.5.0+ supports TCP candidates and TURNS, which is the fallback on
  restrictive networks. **Gotcha**: v4's `AuthHandler` returns `([]byte, bool)`
  and wants a *key* from `turn.GenerateAuthKey`, not a password — older
  `(string, bool)` snippets won't compile.
- This adds UDP relay ports to the deployment surface (`docker run -p`), which
  the current HTTP-only image doesn't have.
- Keep the device-list page and `/api/devices`; "online" now means "has a live
  signaling socket" rather than "sent a frame in the last 15s".

### 6. Browser

Rewrite
[backend/internal/httpapi/ui/templates/view.html](backend/internal/httpapi/ui/templates/view.html):
drop hls.js and `<video>`, use `<canvas>` plus `RTCPeerConnection`.

- Browser creates the offer; SDP/ICE travel over the signaling WebSocket.
- Reassemble chunks per the Start/End flag bits, then `createImageBitmap(blob)`
  → `ctx.drawImage(...)`. `createImageBitmap` decodes off the main thread, which
  matters at 10fps. Espressif's `webrtc_test.html` is a working reference for
  the reassembly.
- Surface connection state (connecting / direct / relayed via TURN / failed) —
  whether TURN was needed is exactly what you'll want to know when debugging a
  slow stream.
- No recording or playback UI — the page is a live view and nothing else.
- Keep the existing dark styling.

### 7. Docs sweep

Update [ROADMAP.md](ROADMAP.md), [README.md](README.md) and
[backend/README.md](backend/README.md): the Implemented section and the system
descriptions still say HLS/archive/retention/playback and the portal's
auto-record option. Rewrite them to the real-time-only system. Small, but the
docs are what the next reader will use to decide whether "record" is still a
thing.

### Order of work

ROADMAP fix (1) is a two-minute doc change; do it while the testing is fresh.
The spike (2) comes before any implementation — it decides the partition layout
and possibly whether this approach is viable on this hardware at all. Config
and portal (3) is fully independent of WebRTC and testable alone, so land it
next. Then the WebRTC transport (4) with backend (5) and browser (6) together,
since none of those three is testable in isolation. The docs sweep (7) lands
with or after (6).

One thing to settle before starting (4): retest `faccab9` on hardware. It's the
current HEAD, it's untested, and the plan treats `main` as a known-good
fallback — worth knowing whether that's actually true before building on it.

## Verification

Camera init, the config portal and the LED patterns are already confirmed (see
Hardware status), so those aren't retested here. Everything below is either
new or still open.

0. **Power supply first.** Run everything from a known-good standalone 5V
   supply with bulk capacitance, never PC USB serial — the board is already
   known to misbehave on USB (camera + WiFi TX peaks alone). Establish this
   before interpreting any other result, or you'll misattribute brownouts to
   code.
1. **Retest `faccab9` on `main`** to establish whether the fallback baseline
   actually works, and whether the intermittent browser freeze survives.
2. **Spike**: `idf.py size-components` on the stub (esp_peer + camera driver
   only) gives the real esp_peer flash cost. This gates the partition decision.
3. **Build/CI**: push the branch; the `Firmware` workflow must build clean on
   ESP-IDF v6.0.2 / target `esp32`, and the image must fit the chosen slot.
4. **Data-channel-only SDP smoke test** — the riskiest unproven assumption, and
   cheap to check. Confirm a media-less offer/answer (only an `m=application`
   line) negotiates against a real browser before building anything on top.
5. **Signaling**: device appears online on the device list, drops off when
   unplugged.
6. **Direct path**: viewer on the same LAN — canvas paints, and
   `RTCPeerConnection.getStats()` shows a `host`/`srflx` candidate pair
   (confirming STUN did its job and TURN wasn't needed).
7. **TURN path**: viewer on a mobile network — stream works, stats show a
   `relay` pair.
8. **The regression that matters**: watch the per-10s
   `capture=/sent=/failed=/dropped=/heap_free=` telemetry while streaming over
   DTLS. If capture fps sags or `EV-EOF-OVF`/`NO-SOI` appears, PSRAM bus
   contention is back (the failure mode of `201204f` and `faccab9`). With SD
   gone, a regression here is unambiguously the WebRTC stack.
9. Run ~30 minutes continuously — historical failures showed at ~12s and on
   watchdog boundaries, and the current browser freeze is intermittent, so a
   short smoke test proves little.
