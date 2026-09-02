# Backend

Go server that relays WebRTC signaling between browser viewers and ESP32-CAM
devices, and runs an embedded TURN/STUN server for NAT traversal. Media
frames travel peer-to-peer (or through TURN) and never touch the backend -
there is no storage, no ffmpeg, no recording.

## Run

```bash
docker run -d -p 80:80 -p 8080:8080 -p 3478:3478/udp -p 3478:3478/tcp \
  -e TURN_PUBLIC_ADDR=<your-server-ip>:3478 \
  -e TURN_SECRET=<long-random-string> \
  ghcr.io/aha1ge/esp32-surveillance/backend:latest
```

`TURN_PUBLIC_ADDR` is the address peers use to reach the server (IP:port).
Without `TURN_PUBLIC_ADDR` + `TURN_SECRET` the server starts anyway, but
announces no ICE servers: connections only work when the browser can reach
the device's host candidates directly (same LAN).

Or locally (requires Go 1.22+):

```bash
go mod tidy   # generates go.sum - see note below
go run ./cmd/server
```

Locally the web UI is served on `LISTEN_ADDR` (`:8080`) — the extra
`UI_LISTEN_ADDR` listener defaults to disabled outside the Docker image.

## Web UI

Open `http://<backend>/` for the device list (with online status, refreshed
every 5 s). Click a device for its live view page: the browser negotiates a
WebRTC DataChannel with the device through the backend's signaling relay and
renders the JPEG frames onto a canvas. Nothing is embedded from CDNs; the
page works offline.

## Endpoints

| Path | Description |
|---|---|
| `GET /` | Device list page (HTML) |
| `GET /view/{id}` | Per-device live view page (HTML + WebRTC) |
| `GET /api/devices` | JSON list of known devices with online status |
| `GET /signaling/{deviceID}` (WS upgrade) | Device signaling socket: firmware connects, receives offers/ICE, sends answers/ICE |
| `GET /view-signaling/{deviceID}` (WS upgrade) | Viewer signaling socket: browser connects, receives ICE servers, exchanges offer/answer/ICE with the device |
| UDP/TCP `3478` | Embedded TURN relay (coturn-style REST credentials) + STUN on the same port |

One viewer at a time per device; a second viewer is rejected with a "busy"
error until the first disconnects.

## Configuration (environment variables)

| Variable | Default | Meaning |
|---|---|---|
| `LISTEN_ADDR` | `:8080` | HTTP/WS listen address (signaling + API + UI) |
| `UI_LISTEN_ADDR` | `""` (disabled; `:80` in the Docker image) | Optional extra HTTP listener |
| `TURN_LISTEN_ADDR` | `:3478` | TURN/STUN listen address |
| `TURN_PUBLIC_ADDR` | `""` | IP:port announced to peers; empty disables TURN |
| `TURN_SECRET` | `""` | REST-credential secret; empty disables TURN |
| `TURN_CRED_HOURS` | `2` | Lifetime of the per-session TURN credentials minted for each viewer |

## Why there's no `go.sum`

Generating a real `go.sum` requires actually resolving dependencies against
the module proxy - a hand-written one would just be fabricated checksums.
CI runs `go mod tidy` before building/testing, which produces a real one; the
Dockerfile's build stage does the same for the same reason. Run `go mod tidy`
once locally before your first local build.
