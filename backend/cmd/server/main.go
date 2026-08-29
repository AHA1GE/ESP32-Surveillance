package main

import (
	"context"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/config"
	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/httpapi"
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

	cleaner := retention.New(reg, cfg.ArchiveRetentionDays, cfg.RetentionCheckInterval)
	cleaner.Start()

	mux := http.NewServeMux()
	mux.HandleFunc("GET /live/{id}/{file}", liveHandler.ServeHTTP)
	mux.HandleFunc("GET /archive/{id}", archiveHandler.ListHandler)
	mux.HandleFunc("GET /archive/{id}/{file}", archiveHandler.ServeHTTP)
	mux.HandleFunc("GET /ingest/{deviceID}", ingester.ServeHTTP)

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

	log.Println("server stopped")
}
