package registry

import (
	"fmt"
	"regexp"
)

// deviceIDPattern is deliberately conservative: device IDs appear in
// filesystem paths (storage/<id>/live|archive), URLs, and HTML templates,
// so only alphanumerics and dashes are allowed. Firmware IDs are
// "esp32cam-XXXXXX" (lowercase hex suffix) and match.
var deviceIDPattern = regexp.MustCompile(`^[a-zA-Z0-9-]{1,64}$`)

func ValidDeviceID(id string) bool {
	return deviceIDPattern.MatchString(id)
}

func ValidateDeviceID(id string) error {
	if ValidDeviceID(id) {
		return nil
	}
	return fmt.Errorf("invalid deviceID %q: must match %s", id, deviceIDPattern)
}
