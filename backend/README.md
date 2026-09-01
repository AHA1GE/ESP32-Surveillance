# Backend

Go server that accepts a WebSocket stream of JPEG frames from one or more
ESP32-CAM devices, transcodes each device's stream with a per-device `ffmpeg`
subprocess, and serves both a live HLS view and a browsable MP4 archive, plus
a built-in web UI.

## Run

```bash
docker run -d -p 80:80 -p 8080:8080 \
  -v surveillance-storage:/storage \
  ghcr.io/aha1ge/esp32-surveillance/backend:latest
```

`/storage` inside the container is the mount point for all recorded clips and
live stream data. The example uses a named volume `surveillance-storage`
(survives container replacement); use `-v ./data:/storage` for a local-folder
bind mount instead.

Or locally (requires Go 1.22+ and `ffmpeg` on PATH):

```bash
go mod tidy   # generates go.sum - see note below
go run ./cmd/server
```

Locally the web UI is served on `LISTEN_ADDR` (`:8080`) — the extra
`UI_LISTEN_ADDR` listener defaults to disabled outside the Docker image.

## Web UI

Open `http://<backend>/` for the device list (with online status, refreshed
every 5 s). Click a device for its live view page: an HLS player plus the
recorded clip list with download links. The player script (hls.js, embedded
in the binary) needs no internet access. To upgrade hls.js:

```bash
curl -L -o internal/httpapi/ui/assets/hls.light.min.js \
  https://cdn.jsdelivr.net/npm/hls.js@1.7.1/dist/hls.light.min.js
# then re-add the Apache-2.0 license banner at the top (see the current file)
```

## Endpoints

| Path | Description |
|---|---|
| `GET /` | Device list page (HTML) |
| `GET /view/{id}` | Per-device page: live HLS player + recorded clip list (HTML) |
| `GET /api/devices` | JSON list of known devices with online status |
| `GET /static/hls.light.min.js` | Vendored hls.js bundle (embedded in the binary) |
| `GET /ingest/{deviceID}` (WS upgrade) | Where firmware connects and pushes binary JPEG frames |
| `GET /live/{id}/{file}` | HLS manifest (`index.m3u8`) and segments for near-real-time viewing |
| `GET /archive/{id}` | JSON list of recorded clips for a device |
| `GET /archive/{id}/{file}` | Download a specific recorded `.mp4` clip |

Point VLC or hls.js at `http://<backend>:8080/live/<deviceID>/index.m3u8` for
live viewing.

## Configuration (environment variables)

| Variable | Default | Meaning |
|---|---|---|
| `LISTEN_ADDR` | `:8080` | HTTP listen address |
| `UI_LISTEN_ADDR` | `""` (disabled; `:80` in the Docker image) | Optional extra HTTP listener for the web UI |
| `STORAGE_ROOT` | `./storage` | Root directory for per-device live/archive data |
| `FFMPEG_PATH` | `ffmpeg` | ffmpeg binary path/name |
| `HLS_SEGMENT_SECONDS` | `4` | Live HLS segment duration |
| `HLS_LIVE_WINDOW_SEGMENTS` | `10` | Number of segments kept in the live sliding window |
| `ARCHIVE_SEGMENT_SECONDS` | `300` | Archive clip duration (independent of the live window) |
| `ARCHIVE_RETENTION_DAYS` | `7` | Archive files older than this are deleted |
| `RETENTION_CHECK_MINUTES` | `5` | How often the retention sweep runs |

## Why there's no `go.sum`

Generating a real `go.sum` requires actually resolving dependencies against
the module proxy - a hand-written one would just be fabricated checksums.
CI runs `go mod tidy` before building/testing, which produces a real one; the
Dockerfile's build stage does the same for the same reason. Run `go mod tidy`
once locally before your first local build.
