// Package ui serves the built-in web UI: a device list page, a per-device
// live view page, and the vendored hls.js bundle, all embedded in the binary
// so the UI works with no internet access.
package ui

import (
	"embed"
	"encoding/json"
	"html/template"
	"log"
	"net/http"
	"sort"
	"time"

	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/pipeline"
	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/registry"
)

//go:embed assets templates
var staticFS embed.FS

// onlineThreshold is how recently a frame must have arrived for a device to
// count as online. Cameras push frames continuously while connected, so
// 15 s means a device only drops offline after a sustained outage.
const onlineThreshold = 15 * time.Second

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
	LastSeen string `json:"lastSeen"` // RFC3339 UTC; "" if never streamed
}

type devicesPage struct {
	Devices []deviceInfo
}

type viewPage struct {
	ID        string
	Online    bool
	StreamURL string
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
	info := makeDeviceInfo(dev)
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := h.tmplView.Execute(w, viewPage{
		ID:        dev.ID,
		Online:    info.Online,
		StreamURL: "/live/" + dev.ID + "/index.m3u8",
	}); err != nil {
		log.Printf("render view page: %v", err)
	}
}

// DevicesJSON serves "GET /api/devices".
func (h *Handler) DevicesJSON(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(h.deviceInfos())
}

// StaticHLS serves the vendored hls.js bundle at "GET /static/hls.light.min.js".
func (h *Handler) StaticHLS(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/javascript; charset=utf-8")
	w.Header().Set("Cache-Control", "public, max-age=86400")
	http.ServeFileFS(w, r, staticFS, "assets/hls.light.min.js")
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

func makeDeviceInfo(dev *pipeline.Device) deviceInfo {
	lastFrame := dev.LastFrame()
	online := !lastFrame.IsZero() && time.Since(lastFrame) < onlineThreshold
	lastSeen := ""
	if !lastFrame.IsZero() {
		lastSeen = lastFrame.UTC().Format(time.RFC3339)
	}
	return deviceInfo{ID: dev.ID, Online: online, LastSeen: lastSeen}
}
