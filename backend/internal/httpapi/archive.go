package httpapi

import (
	"encoding/json"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/registry"
)

type ArchiveHandler struct {
	registry *registry.Registry
}

type ArchiveFile struct {
	Name  string `json:"name"`
	Mtime int64  `json:"mtime"`
	Size  int64  `json:"size"`
}

func NewArchiveHandler(reg *registry.Registry) *ArchiveHandler {
	return &ArchiveHandler{registry: reg}
}

func (h *ArchiveHandler) ListHandler(w http.ResponseWriter, r *http.Request) {
	deviceID := r.PathValue("id")
	if deviceID == "" {
		http.Error(w, "missing id parameter", http.StatusBadRequest)
		return
	}

	dev := h.registry.Get(deviceID)
	if dev == nil {
		http.Error(w, "device not found", http.StatusNotFound)
		return
	}

	entries, err := os.ReadDir(dev.ArchiveDir())
	if err != nil {
		http.Error(w, "failed to read archive", http.StatusInternalServerError)
		log.Printf("failed to read archive dir: %v", err)
		return
	}

	var files []ArchiveFile
	for _, entry := range entries {
		if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".mp4") {
			info, err := entry.Info()
			if err != nil {
				continue
			}
			files = append(files, ArchiveFile{
				Name:  entry.Name(),
				Mtime: info.ModTime().Unix(),
				Size:  info.Size(),
			})
		}
	}

	sort.Slice(files, func(i, j int) bool {
		return files[i].Mtime > files[j].Mtime
	})

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(files)
}

func (h *ArchiveHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	deviceID := r.PathValue("id")
	file := r.PathValue("file")

	if deviceID == "" {
		http.Error(w, "missing id parameter", http.StatusBadRequest)
		return
	}

	if file == "" {
		h.ListHandler(w, r)
		return
	}

	if err := validatePath(file); err != nil {
		http.Error(w, "invalid file", http.StatusBadRequest)
		log.Printf("invalid path: %v", err)
		return
	}

	if !strings.HasSuffix(file, ".mp4") {
		http.Error(w, "only .mp4 files allowed", http.StatusBadRequest)
		return
	}

	dev := h.registry.Get(deviceID)
	if dev == nil {
		http.Error(w, "device not found", http.StatusNotFound)
		return
	}

	filePath := filepath.Join(dev.ArchiveDir(), file)

	info, err := os.Stat(filePath)
	if err != nil {
		http.Error(w, "file not found", http.StatusNotFound)
		return
	}

	if info.IsDir() {
		http.Error(w, "is a directory", http.StatusBadRequest)
		return
	}

	w.Header().Set("Content-Type", "video/mp4")
	http.ServeFile(w, r, filePath)
}
