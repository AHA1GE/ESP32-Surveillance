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

1. `cd esp32-firmware && idf.py menuconfig` -> "ESP32 Surveillance Firmware
   Configuration" - set your WiFi SSID/password, the backend host:port, and
   a unique device ID for this camera.
2. Build via CI: push/open a PR and download the `firmware` artifact from
   the `Firmware` workflow run (or run `idf.py build` locally if you have
   the ESP-IDF toolchain installed).
3. Flash once over USB: `esptool.py write_flash @flash_args` from the
   downloaded build, or `idf.py -p <PORT> flash` locally.

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
