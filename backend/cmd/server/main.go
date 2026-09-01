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
	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/httpapi"
	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/httpapi/ui"
	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/ingest"
	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/pipeline"
	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/registry"
	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/retention"
)

func main() {
	cfg := config.Load()

	if err := os.MkdirAll(cfg.StorageRoot, 0755); err != nil {
		log.Fatalf("failed to create storage root: %v", err)
	}

	reg := registry.New()

	pipelineCfg := &pipeline.DeviceConfig{
		FFmpegPath:            cfg.FFmpegPath,
		StorageRoot:           cfg.StorageRoot,
		HLSSegmentSeconds:     cfg.HLSSegmentSeconds,
		HLSLiveWindowSegments: cfg.HLSLiveWindowSegments,
		ArchiveSegmentSeconds: cfg.ArchiveSegmentSeconds,
	}

	ingester := ingest.New(reg, pipelineCfg)
	liveHandler := httpapi.NewLiveHandler(reg)
	archiveHandler := httpapi.NewArchiveHandler(reg)
	uiHandler := ui.New(reg)

	cleaner := retention.New(reg, cfg.ArchiveRetentionDays, cfg.RetentionCheckInterval)
	cleaner.Start()

	mux := http.NewServeMux()
	mux.HandleFunc("GET /live/{id}/{file}", liveHandler.ServeHTTP)
	mux.HandleFunc("GET /archive/{id}", archiveHandler.ListHandler)
	mux.HandleFunc("GET /archive/{id}/{file}", archiveHandler.ServeHTTP)
	mux.HandleFunc("GET /ingest/{deviceID}", ingester.ServeHTTP)
	mux.HandleFunc("GET /{$}", uiHandler.DeviceList)
	mux.HandleFunc("GET /view/{id}", uiHandler.DeviceView)
	mux.HandleFunc("GET /api/devices", uiHandler.DevicesJSON)
	mux.HandleFunc("GET /static/hls.light.min.js", uiHandler.StaticHLS)

	server := &http.Server{
		Addr:    cfg.ListenAddr,
		Handler: mux,
	}

	log.Printf("starting server on %s", cfg.ListenAddr)
	log.Printf("storage root: %s", cfg.StorageRoot)
	log.Printf("ffmpeg path: %s", cfg.FFmpegPath)

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

	cleaner.Stop()

	devices := reg.All()
	for _, dev := range devices {
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
