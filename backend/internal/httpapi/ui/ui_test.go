package ui

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/registry"
)

func newHandler(t *testing.T) *Handler {
	t.Helper()
	return New(registry.New())
}

func TestDeviceListEmpty(t *testing.T) {
	h := newHandler(t)
	req := httptest.NewRequest(http.MethodGet, "/", nil)
	rec := httptest.NewRecorder()

	h.DeviceList(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rec.Code)
	}
	if !strings.Contains(rec.Body.String(), "No devices have connected yet") {
		t.Errorf("body does not contain the empty-state message: %q", rec.Body.String())
	}
}

func TestDevicesJSONEmpty(t *testing.T) {
	h := newHandler(t)
	req := httptest.NewRequest(http.MethodGet, "/api/devices", nil)
	rec := httptest.NewRecorder()

	h.DevicesJSON(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rec.Code)
	}
	if ct := rec.Header().Get("Content-Type"); ct != "application/json" {
		t.Errorf("Content-Type = %q, want application/json", ct)
	}
	if body := rec.Body.String(); body != "[]\n" {
		t.Errorf("body = %q, want []", body)
	}
}

func TestUnknownDeviceView(t *testing.T) {
	h := newHandler(t)
	req := httptest.NewRequest(http.MethodGet, "/view/does-not-exist", nil)
	rec := httptest.NewRecorder()

	h.DeviceView(rec, req)

	if rec.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", rec.Code)
	}
}

func TestWithDevice(t *testing.T) {
	reg := registry.New()
	if _, err := reg.GetOrCreate("esp32cam-test01"); err != nil {
		t.Fatalf("GetOrCreate: %v", err)
	}

	h := New(reg)
	req := httptest.NewRequest(http.MethodGet, "/api/devices", nil)
	rec := httptest.NewRecorder()
	h.DevicesJSON(rec, req)

	var infos []deviceInfo
	if err := json.Unmarshal(rec.Body.Bytes(), &infos); err != nil {
		t.Fatalf("unmarshal /api/devices: %v", err)
	}
	if len(infos) != 1 || infos[0].ID != "esp32cam-test01" {
		t.Fatalf("infos = %+v, want one entry for esp32cam-test01", infos)
	}
	// Registered but never attached: offline, no last-seen timestamp.
	if infos[0].Online {
		t.Errorf("device with no signaling socket reported online: %+v", infos[0])
	}
	if infos[0].LastSeen != "" {
		t.Errorf("never-connected device has a lastSeen: %q", infos[0].LastSeen)
	}

	req = httptest.NewRequest(http.MethodGet, "/view/esp32cam-test01", nil)
	rec = httptest.NewRecorder()
	h.DeviceView(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("view page status = %d, want 200", rec.Code)
	}
	if !strings.Contains(rec.Body.String(), "esp32cam-test01") {
		t.Error("view page does not contain the device ID")
	}
}
