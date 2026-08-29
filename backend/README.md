# Backend

Go server that accepts a WebSocket stream of JPEG frames from one or more
ESP32-CAM devices, transcodes each device's stream with a per-device `ffmpeg`
subprocess, and serves both a live HLS view and a browsable MP4 archive.

## Run

```bash
docker run -p 8080:8080 -v ./data:/storage ghcr.io/aha1ge/esp32-surveillance/backend:latest
```

Or locally (requires Go 1.22+ and `ffmpeg` on PATH):

```bash
go mod tidy   # generates go.sum - see note below
go run ./cmd/server
```

## Endpoints

| Path | Description |
|---|---|
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
