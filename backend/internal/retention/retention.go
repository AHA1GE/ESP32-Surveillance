package retention

import (
	"log"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/registry"
)

type Cleaner struct {
	registry       *registry.Registry
	retentionDays  int
	checkInterval  time.Duration
	stopCh         chan struct{}
	stoppedCh      chan struct{}
}

func New(reg *registry.Registry, retentionDays int, checkInterval time.Duration) *Cleaner {
	return &Cleaner{
		registry:      reg,
		retentionDays: retentionDays,
		checkInterval: checkInterval,
		stopCh:        make(chan struct{}),
		stoppedCh:     make(chan struct{}),
	}
}

func (c *Cleaner) Start() {
	go c.run()
}

func (c *Cleaner) Stop() {
	close(c.stopCh)
	<-c.stoppedCh
}

func (c *Cleaner) run() {
	defer close(c.stoppedCh)

	ticker := time.NewTicker(c.checkInterval)
	defer ticker.Stop()

	for {
		select {
		case <-c.stopCh:
			return
		case <-ticker.C:
			c.cleanup()
		}
	}
}

func (c *Cleaner) cleanup() {
	devices := c.registry.All()
	cutoff := time.Now().AddDate(0, 0, -c.retentionDays)

	for _, dev := range devices {
		entries, err := os.ReadDir(dev.ArchiveDir())
		if err != nil {
			log.Printf("failed to read archive dir for device %s: %v", dev.ID, err)
			continue
		}

		for _, entry := range entries {
			if entry.IsDir() {
				continue
			}

			if !strings.HasSuffix(entry.Name(), ".mp4") {
				continue
			}

			info, err := entry.Info()
			if err != nil {
				continue
			}

			if info.ModTime().Before(cutoff) {
				filePath := filepath.Join(dev.ArchiveDir(), entry.Name())
				if err := os.Remove(filePath); err != nil {
					log.Printf("failed to delete %s: %v", filePath, err)
				} else {
					log.Printf("deleted old archive file: %s", filePath)
				}
			}
		}
	}
}
