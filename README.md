# ESP32 Surveillance Platform

A surveillance platform using one or more ESP32-CAM modules (OV2640) and a
backend server that turns their live JPEG streams into a near-real-time
viewable stream plus a browsable recorded history.

## Architecture

- **Firmware** ([esp32-firmware/](esp32-firmware/)) - native ESP-IDF (C), not Arduino. Captures
  JPEG frames from the OV2640 and pushes them over an outbound WebSocket
  connection to the backend. Built entirely by CI - see below.
- **Backend** ([backend/](backend/)) - Go. Accepts WebSocket connections from any number of
  devices, feeds each device's frames into a dedicated `ffmpeg` process, and
  serves a live HLS stream plus a recorded MP4 archive per device. Ships as a
  Docker image.
- **CI** ([.github/workflows/](.github/workflows/)) - GitHub Actions is the only build path for both
  sides. Firmware builds produce a downloadable `.bin`; tag pushes also
  attach it to a GitHub Release. Backend builds run `go vet`/`go test` and
  publish a multi-arch Docker image to GHCR.

Each camera dials out to the backend rather than the backend polling
cameras - that's what lets multiple devices join without any backend-side
configuration, and lets a camera keep working across DHCP lease changes.

## Getting started

### Firmware

Device settings are no longer compile-time: each camera stores them in NVS
and configures itself through a web portal served from its own WiFi access
point.

1. Build via CI: push/open a PR and download the `firmware` artifact from
   the `Firmware` workflow run (or run `idf.py build` locally if you have
   the ESP-IDF toolchain installed).
2. Flash once over USB: `esptool.py write_flash @flash_args` from the
   downloaded build, or `idf.py -p <PORT> flash` locally.
3. First boot (or any boot with missing/invalid config) opens the config
   portal: the red LED double-blinks and the camera serves an open WiFi AP
   named `ESP32-CAM-XXXXXX`. Join it, open `http://192.168.4.1/`, fill in
   the WiFi SSID/password and backend host:port, and press Save and Reboot.
4. With valid config the camera joins your WiFi and streams to the backend.

Status LED (red, on-board): solid = streaming to a connected backend; fast
flash = error (camera init failure, backend unreachable); double blink =
config portal mode. If the stored WiFi credentials stop working, the camera
automatically reopens the portal after ~60 s so it can be reconfigured
without a reflash.

Notes: the device ID is derived from the WiFi MAC (`esp32cam-XXXXXX`, the
same suffix as the AP name) and shows up in the backend live/archive URLs.
`auto_record`/`auto_flash` are stored for future use but do nothing yet, and
the WiFi password is stored in plaintext in NVS (no flash encryption yet).

Firmware updates are manual for now (reflash over USB). The OTA path
(`main/ota.c`) is implemented but inert - enabling `CONFIG_ENABLE_AUTO_OTA`
plus wiring a periodic timer is the intended way to switch a device over to
self-updating from the latest GitHub Release later, without further
restructuring.

### Backend

```bash
docker run -p 8080:8080 -v ./data:/storage ghcr.io/aha1ge/esp32-surveillance/backend:latest
```

See [backend/README.md](backend/README.md) for configuration and endpoints.
Once a device is streaming, open `http://<backend>:8080/live/<deviceID>/index.m3u8`
in VLC (or any HLS-capable player) to watch live; recorded clips are listed
at `http://<backend>:8080/archive/<deviceID>`.

## Roadmap

See [ROADMAP.md](ROADMAP.md).

## Contributing

Contributions and feedback are welcome. Please open an issue or submit a
pull request with your suggestions.

## License

This project is licensed under the MIT License.
