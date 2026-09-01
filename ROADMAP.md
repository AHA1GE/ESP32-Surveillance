# Roadmap

The platform was rebuilt from scratch on native ESP-IDF firmware and a Go
backend, both built exclusively via GitHub Actions. The milestones below
were implemented together as a single rearchitecture; hardware verification
(marked separately) still needs a real device.

## Implemented

- [x] ESP-IDF firmware: WiFi, OV2640 capture (PSRAM, dual frame buffer),
      outbound WebSocket streaming to the backend.
- [x] OTA-capable partition table (two OTA slots) from day one, so enabling
      OTA later never requires a reflash just to change the partition layout.
- [x] Go backend: per-device WebSocket ingest, one `ffmpeg` process per
      device piping frames straight to disk-backed output (no intermediate
      loose JPEG files).
- [x] Live viewing via a rolling HLS window per device.
- [x] Recorded history via independent archive segments plus a retention
      sweep.
- [x] Multi-device support: devices are auto-registered by ID on first
      connection, no backend configuration needed per camera.
- [x] OTA stub: `ota_check_latest_release()`/`ota_apply()` implemented
      against the GitHub Releases API, gated inert behind
      `CONFIG_ENABLE_AUTO_OTA` (default off).
- [x] Persistent device config in NVS + AP web portal: WiFi/backend
      settings are configured at runtime (open AP `ESP32-CAM-XXXXXX` with a
      pure-HTML form at `http://192.168.4.1/`), no reflash to reconfigure.
      Device ID is derived from the WiFi MAC; status LED on GPIO33 (solid =
      streaming, double blink = portal, fast flash = error); STA connect
      timeout falls back to the portal.
- [x] CI: firmware builds on every push and attaches the `.bin` to a GitHub
      Release on tag push; backend tests + publishes a multi-arch Docker
      image to GHCR.

## Needs hardware to verify

- [x] Flash a real AI-Thinker ESP32-CAM and confirm camera init succeeds
      with PSRAM enabled (this is the setting the old Arduino firmware got
      wrong).
- [x] Confirm the config portal flow on a real device: first flash -> AP +
      form -> save -> reboot -> streaming, plus the ~60 s STA-timeout
      fallback to the portal with wrong credentials.
- [x] Confirm the GPIO33 LED patterns (solid / double blink / fast flash)
      and that the portal form renders correctly on a phone browser.
- [ ] Confirm sustained frame rate/quality over real WiFi into the live HLS
      view.
- [ ] Run two physical devices with different device IDs simultaneously and
      confirm independent live/archive output.
- [ ] Tag a release, confirm the GitHub Release gets the `.bin` attached,
      and manually exercise `ota_apply()` once before ever enabling
      `CONFIG_ENABLE_AUTO_OTA` unattended.

## Future

- [ ] Wire `CONFIG_ENABLE_AUTO_OTA` to an actual periodic timer instead of a
      one-shot check at boot.
- [ ] Authentication in front of the live/archive HTTP endpoints - there is
      currently none, matching the old system's scope but worth revisiting
      before exposing this beyond a trusted home LAN.
- [ ] Cloud/serverless storage backends, if scale ever demands it.
