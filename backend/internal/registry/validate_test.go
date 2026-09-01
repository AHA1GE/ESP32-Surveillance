package registry

import (
	"strings"
	"testing"
)

func TestValidDeviceID(t *testing.T) {
	valid := []string{
		"esp32cam-abc123",
		"ESP32CAM-ABC123",
		"a",
		strings.Repeat("a", 64),
	}
	for _, id := range valid {
		if !ValidDeviceID(id) {
			t.Errorf("ValidDeviceID(%q) = false, want true", id)
		}
	}

	invalid := []string{
		"",
		"../evil",
		"a/b",
		"a\\b",
		"a b",
		"esp32cam_abc", // underscore
		strings.Repeat("a", 65),
		"café",
	}
	for _, id := range invalid {
		if ValidDeviceID(id) {
			t.Errorf("ValidDeviceID(%q) = true, want false", id)
		}
		if err := ValidateDeviceID(id); err == nil {
			t.Errorf("ValidateDeviceID(%q) = nil, want error", id)
		}
	}
}
