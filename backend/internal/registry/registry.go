package registry

import (
	"sync"

	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/pipeline"
)

type Registry struct {
	devices map[string]*pipeline.Device
	mu      sync.Mutex
}

func New() *Registry {
	return &Registry{
		devices: make(map[string]*pipeline.Device),
	}
}

func (r *Registry) GetOrCreate(deviceID string, cfg *pipeline.DeviceConfig) (*pipeline.Device, error) {
	if err := ValidateDeviceID(deviceID); err != nil {
		return nil, err
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	if dev, ok := r.devices[deviceID]; ok {
		return dev, nil
	}

	dev, err := pipeline.NewDevice(deviceID, cfg)
	if err != nil {
		return nil, err
	}

	r.devices[deviceID] = dev
	return dev, nil
}

func (r *Registry) Get(deviceID string) *pipeline.Device {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.devices[deviceID]
}

func (r *Registry) All() []*pipeline.Device {
	r.mu.Lock()
	defer r.mu.Unlock()

	devs := make([]*pipeline.Device, 0, len(r.devices))
	for _, dev := range r.devices {
		devs = append(devs, dev)
	}
	return devs
}
