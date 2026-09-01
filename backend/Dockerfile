# Build stage
FROM golang:1.22-alpine AS builder

WORKDIR /build

COPY go.mod go.mod
COPY cmd cmd
COPY internal internal

# go.sum is intentionally not committed (see backend/README.md) - go mod tidy
# resolves it fresh here rather than assuming a pinned checksum file exists.
RUN go mod tidy && \
    CGO_ENABLED=0 GOOS=linux go build -o server ./cmd/server

# Runtime stage
FROM alpine:3.20

RUN apk add --no-cache ffmpeg ca-certificates

WORKDIR /app

COPY --from=builder /build/server .

ENV LISTEN_ADDR=:8080 \
    UI_LISTEN_ADDR=:80 \
    STORAGE_ROOT=/storage \
    FFMPEG_PATH=ffmpeg \
    HLS_SEGMENT_SECONDS=4 \
    HLS_LIVE_WINDOW_SEGMENTS=10 \
    ARCHIVE_SEGMENT_SECONDS=300 \
    ARCHIVE_RETENTION_DAYS=7 \
    RETENTION_CHECK_MINUTES=5

VOLUME ["/storage"]

EXPOSE 80 8080

CMD ["./server"]
