# Roadmap

The platform was rebuilt from scratch on native ESP-IDF firmware and a Go
backend, both built exclusively via GitHub Actions. The milestones below
were implemented together as a single rearchitecture; hardware verification
(marked separately) still needs a real device.

The system is real-time only: live WebRTC viewing, no recording and no
playback (see WEBRTC_PLAN.md for the rationale and design).

## Implemented

- [x] ESP-IDF firmware: WiFi, OV2640 capture (PSRAM, dual frame buffer).
- [x] Firmware WebRTC: esp_peer DataChannel (DTLS-SRTP + SCTP) carrying
      chunked JPEG frames, negotiated over a WebSocket signaling connection
      to the backend; `esp_log()` compat shim for IDF v5.x.
- [x] Go backend: WebRTC signaling relay (offer/answer/ICE between browser
      and device, one viewer per device) with an embedded TURN/STUN server
      (pion/turn) for NAT traversal. No ffmpeg, no storage.
- [x] Browser viewer: `/view/{id}` negotiates a WebRTC connection in the
      browser and renders the incoming JPEG frames onto a canvas.
- [x] Multi-device support: devices are auto-registered by ID on first
      connection, no backend configuration needed per camera.
- [x] OTA-capable partition table (two OTA slots) from day one, so enabling
      OTA later never requires a reflash just to change the partition layout.
- [x] OTA stub: `ota_check_latest_release()`/`ota_apply()` implemented
      against the GitHub Releases API, gated inert behind
      `CONFIG_ENABLE_AUTO_OTA` (default off).
- [x] Persistent device config in NVS + AP web portal: WiFi/backend/TURN
      settings are configured at runtime (open AP `ESP32-CAM-XXXXXX` with a
      pure-HTML form at `http://192.168.4.1/`), no reflash to reconfigure.
      Device ID is derived from the WiFi MAC; status LED on GPIO33 (solid =
      signaling connected, double blink = portal, fast flash = error); STA
      connect timeout falls back to the portal.
- [x] CI: firmware builds on every push and attaches the `.bin` to a GitHub
      Release on tag push; backend tests + publishes a multi-arch Docker
      image to GHCR.

## Needs hardware to verify

- [x] Flash a real AI-Thinker ESP32-CAM and confirm camera init succeeds
      with PSRAM enabled (this is the setting the old Arduino firmware got
      wrong).
- [x] Confirm the config portal flow on a real device: first flash -> AP +
      form -> save -> reboot -> streaming.
- [ ] Confirm the ~60 s STA-timeout fallback to the portal with wrong
      credentials.
- [x] Confirm the GPIO33 LED patterns (solid / double blink / fast flash)
      and that the portal form renders correctly on a phone browser.
- [ ] Confirm a WebRTC view session connects and streams sustained
      frame rate/quality over real WiFi (direct and TURN-relayed paths).
- [ ] Verify the datachannel resync logic: force frame drops (busy WiFi,
      TURN stall) and confirm the canvas recovers on the next keyframe
      without freezing.
- [ ] Run two physical devices with different device IDs simultaneously and
      confirm independent live views.
- [ ] Tag a release, confirm the GitHub Release gets the `.bin` attached,
      and manually exercise `ota_apply()` once before ever enabling
      `CONFIG_ENABLE_AUTO_OTA` unattended.

Hardware findings from 2026-09-02 testing: the board streams only when
standalone-powered (fails on PC USB - the classic brownout signature; use a
known-good 5V supply with bulk capacitance). The intermittent browser freeze
at `201204f` was on the old WebSocket+HLS path; the WebRTC rewrite above is
the fix attempt, still to be confirmed on hardware.

## Future

- [ ] Wire `CONFIG_ENABLE_AUTO_OTA` to an actual periodic timer instead of a
      one-shot check at boot.
- [ ] Authentication in front of the signaling/view endpoints - there is
      currently none, matching the old system's scope but worth revisiting
      before exposing this beyond a trusted home LAN.
- [ ] Multi-viewer support per device (fan the datachannel out server-side
      or negotiate one session per viewer).
- [ ] Recording, if it is ever wanted back: it was deliberately removed in
      favor of real-time-only simplicity; reintroducing it means revisiting
      the storage/retention architecture from scratch.
