// Package ui serves the built-in web UI: a device list page and a
// per-device live view page, embedded in the binary so the UI works with
// no internet access. Video never touches this server - view.html opens a
// WebRTC DataChannel straight to the camera and draws JPEG frames itself.
package ui

import (
	"embed"
	"encoding/json"
	"html/template"
	"log"
	"net/http"
	"sort"
	"time"

	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/registry"
)

//go:embed templates
var staticFS embed.FS

type Handler struct {
	reg         *registry.Registry
	tmplDevices *template.Template
	tmplView    *template.Template
}

func New(reg *registry.Registry) *Handler {
	return &Handler{
		reg: reg,
		// Embedded templates are static; a parse failure is a build bug.
		tmplDevices: template.Must(template.ParseFS(staticFS, "templates/devices.html")),
		tmplView:    template.Must(template.ParseFS(staticFS, "templates/view.html")),
	}
}

type deviceInfo struct {
	ID       string `json:"id"`
	Online   bool   `json:"online"`
	LastSeen string `json:"lastSeen"` // RFC3339 UTC; "" if never connected
}

type devicesPage struct {
	Devices []deviceInfo
}

type viewPage struct {
	ID     string
	Online bool
}

// DeviceList renders the device list page for "GET /{$}".
func (h *Handler) DeviceList(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := h.tmplDevices.Execute(w, devicesPage{Devices: h.deviceInfos()}); err != nil {
		log.Printf("render devices page: %v", err)
	}
}

// DeviceView renders the single-device page for "GET /view/{id}".
func (h *Handler) DeviceView(w http.ResponseWriter, r *http.Request) {
	dev := h.reg.Get(r.PathValue("id"))
	if dev == nil {
		http.Error(w, "device not found", http.StatusNotFound)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := h.tmplView.Execute(w, viewPage{
		ID:     dev.ID,
		Online: dev.Online(),
	}); err != nil {
		log.Printf("render view page: %v", err)
	}
}

// DevicesJSON serves "GET /api/devices".
func (h *Handler) DevicesJSON(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(h.deviceInfos())
}

func (h *Handler) deviceInfos() []deviceInfo {
	devs := h.reg.All()
	infos := make([]deviceInfo, 0, len(devs))
	for _, dev := range devs {
		infos = append(infos, makeDeviceInfo(dev))
	}
	// Map iteration order is random; sort so the list page doesn't flicker.
	sort.Slice(infos, func(i, j int) bool { return infos[i].ID < infos[j].ID })
	return infos
}

func makeDeviceInfo(dev *registry.Device) deviceInfo {
	lastSeen := ""
	if t := dev.LastSeen(); !t.IsZero() {
		lastSeen = t.UTC().Format(time.RFC3339)
	}
	return deviceInfo{ID: dev.ID, Online: dev.Online(), LastSeen: lastSeen}
}
