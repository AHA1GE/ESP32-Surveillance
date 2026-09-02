# ESP32 Surveillance Platform

A real-time surveillance platform using one or more ESP32-CAM modules and a backend server that relays WebRTC connections from browser to
device. Live viewing only - no recording, no playback.

## Architecture

- **Firmware** ([esp32-firmware/](esp32-firmware/)) - native ESP-IDF (C), not Arduino. Captures
  JPEG frames from the OV2640 and sends them over a WebRTC DataChannel
  (esp_peer: DTLS + SCTP, chunked frames). Signaling (offer/answer/ICE) runs
  over an outbound WebSocket to the backend. Built entirely by CI - see
  below.
- **Backend** ([backend/](backend/)) - Go. Relays WebRTC signaling between browsers and
  devices (one viewer per device), and runs an embedded TURN/STUN server for
  NAT traversal. Stateless w.r.t. media: frames never touch the backend.
  Ships as a Docker image.
- **Viewer** ([backend/internal/httpapi/ui/templates/view.html](backend/internal/httpapi/ui/templates/view.html)) - the
  browser negotiates a WebRTC connection with the device and renders the
  JPEG frames onto a canvas.
- **CI** ([.github/workflows/](.github/workflows/)) - GitHub Actions is the only build path for both
  sides. Firmware builds produce a downloadable `.bin`; tag pushes also
  attach it to a GitHub Release. Backend builds run `go vet`/`go test` and
  publish a multi-arch Docker image to GHCR.

Each camera dials out to the backend rather than the backend polling
cameras - that's what lets multiple devices join without any backend-side
configuration, and lets a camera keep working across DHCP lease changes.
Media flows peer-to-peer; the backend only moves signaling and (when needed)
TURN-relayed data.

## Getting started

### Firmware

Device settings are no longer compile-time: each camera stores them in NVS
and configures itself through a web portal served from its own WiFi access
point.

1. Build via CI: push/open a PR and download the `firmware` artifact from
   the `Firmware` workflow run (or run `idf.py build` locally if you have
   ESP-IDF v6 or later installed — the firmware refuses older IDFs).
2. Flash once over USB: `esptool.py write_flash @flash_args` from the
   downloaded build, or `idf.py -p <PORT> flash` locally. The build also
   contains `merged-binary.bin`, one image flashable at 0x0:
   `esptool.py -p <PORT> write_flash 0x0 merged-binary.bin`.
3. First boot (or any boot with missing/invalid config) opens the config
   portal: the red LED double-blinks and the camera serves an open WiFi AP
   named `ESP32-CAM-XXXXXX`. Join it, open `http://192.168.4.1/`, fill in
   the WiFi SSID/password and backend host:port (and optionally a TURN URL),
   and press Save and Reboot.
4. With valid config the camera joins your WiFi and connects to the backend
   signaling socket; the LED turns solid when the signaling connection is up.

Status LED (red, on-board): solid = signaling connected to the backend; fast
flash = error (camera init failure, backend unreachable); double blink =
config portal mode. If the stored WiFi credentials stop working, the camera
automatically reopens the portal after ~60 s so it can be reconfigured
without a reflash.

Notes: the device ID is derived from the WiFi MAC (`esp32cam-XXXXXX`, the
same suffix as the AP name) and shows up in the backend URLs. The optional
`turn_server` config field accepts a `turn:host:port` URL for setups that
need an external relay instead of (or in addition to) the backend's embedded
one. The WiFi password is stored in plaintext in NVS (no flash encryption
yet).

Firmware updates are manual for now (reflash over USB). The OTA path
(`main/ota.c`) is implemented but inert - enabling `CONFIG_ENABLE_AUTO_OTA`
plus wiring a periodic timer is the intended way to switch a device over to
self-updating from the latest GitHub Release later, without further
restructuring.

### Backend

```bash
docker run -d -p 80:80 -p 8080:8080 -p 3478:3478/udp -p 3478:3478/tcp \
  -e TURN_PUBLIC_ADDR=<your-server-ip>:3478 \
  -e TURN_SECRET=<long-random-string> \
  ghcr.io/aha1ge/esp32-surveillance/backend:latest
```

Set `TURN_PUBLIC_ADDR` to the server's public IP (the port must match the
published `3478`). Without both TURN vars the server still runs and relays
signaling, but remote peers can only connect directly (LAN or host-network
containers); see [backend/README.md](backend/README.md).

Open `http://<backend>/` in a browser for the web UI: a device list with
online status, and a per-device live view page that connects directly to the
camera over WebRTC.

## Roadmap

See [ROADMAP.md](ROADMAP.md).

## Contributing

Contributions and feedback are welcome. Please open an issue or submit a
pull request with your suggestions.

## License

This project is licensed under the MIT License.
