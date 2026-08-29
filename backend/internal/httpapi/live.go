package httpapi

import (
	"fmt"
	"log"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"strings"

	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/registry"
)

type LiveHandler struct {
	registry *registry.Registry
}

func NewLiveHandler(reg *registry.Registry) *LiveHandler {
	return &LiveHandler{registry: reg}
}

func (h *LiveHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	deviceID := r.PathValue("id")
	file := r.PathValue("file")

	if deviceID == "" || file == "" {
		http.Error(w, "missing id or file parameter", http.StatusBadRequest)
		return
	}

	if err := validatePath(file); err != nil {
		http.Error(w, "invalid file", http.StatusBadRequest)
		log.Printf("invalid path: %v", err)
		return
	}

	dev := h.registry.Get(deviceID)
	if dev == nil {
		http.Error(w, "device not found", http.StatusNotFound)
		return
	}

	filePath := filepath.Join(dev.LiveDir(), file)

	info, err := os.Stat(filePath)
	if err != nil {
		http.Error(w, "file not found", http.StatusNotFound)
		return
	}

	if info.IsDir() {
		http.Error(w, "is a directory", http.StatusBadRequest)
		return
	}

	setContentType(w, file)
	http.ServeFile(w, r, filePath)
}

func setContentType(w http.ResponseWriter, file string) {
	ext := strings.ToLower(path.Ext(file))
	switch ext {
	case ".m3u8":
		w.Header().Set("Content-Type", "application/vnd.apple.mpegurl")
	case ".ts":
		w.Header().Set("Content-Type", "video/mp2t")
	}
}

func validatePath(p string) error {
	if strings.Contains(p, "..") {
		return fmt.Errorf("path contains ..")
	}
	if strings.Contains(p, "/") || strings.Contains(p, "\\") {
		return fmt.Errorf("path contains separators")
	}
	return nil
}
