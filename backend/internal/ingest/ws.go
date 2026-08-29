package ingest

import (
	"context"
	"log"
	"net/http"

	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/pipeline"
	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/registry"
	"github.com/coder/websocket"
)

type Ingester struct {
	registry *registry.Registry
	cfg      *pipeline.DeviceConfig
}

func New(reg *registry.Registry, cfg *pipeline.DeviceConfig) *Ingester {
	return &Ingester{
		registry: reg,
		cfg:      cfg,
	}
}

func (i *Ingester) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	deviceID := r.PathValue("deviceID")
	if deviceID == "" {
		http.Error(w, "missing deviceID", http.StatusBadRequest)
		return
	}

	conn, err := websocket.Accept(w, r, nil)
	if err != nil {
		log.Printf("failed to accept WebSocket connection: %v", err)
		return
	}
	defer conn.Close(websocket.StatusNormalClosure, "")

	// coder/websocket defaults to a 32KiB message limit; a single SVGA JPEG
	// frame routinely exceeds that, which would otherwise close the
	// connection on the first oversized frame.
	conn.SetReadLimit(2 << 20)

	dev, err := i.registry.GetOrCreate(deviceID, i.cfg)
	if err != nil {
		log.Printf("failed to create device %s: %v", deviceID, err)
		return
	}

	log.Printf("device %s connected", deviceID)

	ctx := context.Background()
	for {
		msgType, frame, err := conn.Read(ctx)
		if err != nil {
			log.Printf("device %s read error: %v", deviceID, err)
			return
		}

		if msgType != websocket.MessageBinary {
			log.Printf("device %s sent non-binary message, ignoring", deviceID)
			continue
		}

		if err := dev.WriteFrame(frame); err != nil {
			log.Printf("device %s write frame error: %v", deviceID, err)
			return
		}
	}
}
