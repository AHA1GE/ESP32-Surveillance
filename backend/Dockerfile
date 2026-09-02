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

# Runtime stage. No ffmpeg, no storage volume: the server only relays
# signaling and (optionally) TURN traffic. Media flows device -> browser.
FROM alpine:3.20

RUN apk add --no-cache ca-certificates

WORKDIR /app

COPY --from=builder /build/server .

ENV LISTEN_ADDR=:8080 \
    UI_LISTEN_ADDR=:80 \
    TURN_LISTEN_ADDR=:3478 \
    TURN_PUBLIC_ADDR= \
    TURN_SECRET= \
    TURN_CRED_HOURS=2

EXPOSE 80 8080 3478/udp 3478/tcp

CMD ["./server"]
