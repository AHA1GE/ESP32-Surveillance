package main

import (
	"context"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/config"
	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/httpapi/ui"
	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/registry"
	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/signaling"
)

func main() {
	cfg := config.Load()

	reg := registry.New()
	uiHandler := ui.New(reg)

	// ICE server list announced to peers. The TURN server doubles as the
	// STUN server (pion/turn answers STUN binding requests on the same UDP
	// port), so no external STUN dependency exists.
	var stunURLs, turnURLs []string
	if cfg.TURNEnabled() {
		stunURLs = []string{"stun:" + cfg.TURNPublicAddr}
		turnURLs = []string{
			"turn:" + cfg.TURNPublicAddr + "?transport=udp",
			"turn:" + cfg.TURNPublicAddr + "?transport=tcp",
		}
	}

	hub := signaling.New(reg, signaling.Options{
		STUNURLs: stunURLs,
		TURNURLs: turnURLs,
		Secret:   cfg.TURNSecret,
		CredTTL:  cfg.TURNCredTTL(),
	})

	if cfg.TURNEnabled() {
		turnServer, err := signaling.StartTURN(signaling.TURNOptions{
			ListenAddr: cfg.TURNListenAddr,
			PublicAddr: cfg.TURNPublicAddr,
			Secret:     cfg.TURNSecret,
			Realm:      "esp32cam",
		}, log.Default())
		if err != nil {
			log.Fatalf("TURN: %v", err)
		}
		defer func() {
			if err := turnServer.Close(); err != nil {
				log.Printf("TURN close: %v", err)
			}
		}()
	} else {
		log.Printf("TURN disabled (set TURN_SECRET and TURN_PUBLIC_ADDR to enable relay)")
	}

	mux := http.NewServeMux()
	mux.HandleFunc("GET /signaling/{deviceID}", hub.ServeDeviceWS)
	mux.HandleFunc("GET /view-signaling/{deviceID}", hub.ServeViewerWS)
	mux.HandleFunc("GET /{$}", uiHandler.DeviceList)
	mux.HandleFunc("GET /view/{id}", uiHandler.DeviceView)
	mux.HandleFunc("GET /api/devices", uiHandler.DevicesJSON)

	server := &http.Server{
		Addr:    cfg.ListenAddr,
		Handler: mux,
	}

	log.Printf("starting server on %s", cfg.ListenAddr)

	go func() {
		if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("server error: %v", err)
		}
	}()

	// Optional extra listener for the web UI (same mux, so it serves the
	// full API too). Disabled by default; the Docker image sets it to :80.
	var uiServer *http.Server
	if cfg.UIListenAddr != "" {
		uiServer = &http.Server{
			Addr:    cfg.UIListenAddr,
			Handler: mux,
		}
		log.Printf("ui listener on %s", cfg.UIListenAddr)
		go func() {
			if err := uiServer.ListenAndServe(); err != nil && err != http.ErrServerClosed {
				log.Fatalf("ui server error: %v", err)
			}
		}()
	}

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	<-sigCh

	log.Println("shutting down...")

	// Close every device and viewer socket; their read loops run cleanup.
	for _, dev := range reg.All() {
		dev.Close()
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	server.Shutdown(ctx)
	if uiServer != nil {
		if err := uiServer.Shutdown(ctx); err != nil {
			log.Printf("ui server shutdown: %v", err)
		}
	}

	log.Println("server stopped")
}
