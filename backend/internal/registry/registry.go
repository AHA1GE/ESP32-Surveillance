// Package registry is the in-memory table of devices known to this backend.
// It holds no frame data and no durable state: a device entry is its live
// signaling socket, the current viewer session (at most one), and the time
// it was last seen. Real-time only - nothing else exists to track.
package registry

import (
	"context"
	"fmt"
	"log"
	"sync"
	"time"

	"github.com/coder/websocket"
)

// Device is one camera. The signaling socket is owned by the device's read
// loop in the signaling package; writes to either socket are serialized by
// the mutex because messages from both sides (device read loop, viewer read
// loop, session teardown) can be in flight concurrently.
type Device struct {
	ID string

	mu       sync.Mutex
	conn     *websocket.Conn // device's signaling socket; nil while offline
	viewer   *websocket.Conn // active viewer; nil when nobody is watching
	lastSeen time.Time       // last attach or detach; zero = never connected
}

// Attach records that the device's signaling socket came up.
func (d *Device) Attach(conn *websocket.Conn) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.conn = conn
	d.lastSeen = time.Now()
}

// Detach clears the device's signaling socket. Returns the viewer socket to
// evict, if any - a session cannot outlive its device.
func (d *Device) Detach(conn *websocket.Conn) *websocket.Conn {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.conn == conn {
		d.conn = nil
	}
	viewer := d.viewer
	d.viewer = nil
	d.lastSeen = time.Now()
	return viewer
}

// Online reports whether the device currently has a live signaling socket.
func (d *Device) Online() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.conn != nil
}

// LastSeen returns when the device last attached or detached, or the zero
// time if it has never connected since startup.
func (d *Device) LastSeen() time.Time {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.lastSeen
}

// Send writes a text message to the device over its signaling socket.
func (d *Device) Send(ctx context.Context, msg []byte) error {
	d.mu.Lock()
	conn := d.conn
	d.mu.Unlock()
	if conn == nil {
		return fmt.Errorf("device %s offline", d.ID)
	}
	return conn.Write(ctx, websocket.MessageText, msg)
}

// TryClaimViewer enforces the exactly-one-viewer rule. On success the viewer
// socket is stored and returned true; a second concurrent viewer gets false.
func (d *Device) TryClaimViewer(conn *websocket.Conn) bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.viewer != nil {
		return false
	}
	d.viewer = conn
	return true
}

// ReleaseViewer clears the viewer slot if it is still held by conn.
func (d *Device) ReleaseViewer(conn *websocket.Conn) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.viewer == conn {
		d.viewer = nil
	}
}

// SendViewer writes a text message to the current viewer, if any.
func (d *Device) SendViewer(ctx context.Context, msg []byte) error {
	d.mu.Lock()
	viewer := d.viewer
	d.mu.Unlock()
	if viewer == nil {
		return fmt.Errorf("device %s has no viewer", d.ID)
	}
	return viewer.Write(ctx, websocket.MessageText, msg)
}

// Close tears the device down: the device socket (if the device is online)
// and the viewer socket (if a session is live).
func (d *Device) Close() {
	d.mu.Lock()
	deviceConn, viewer := d.conn, d.viewer
	d.conn, d.viewer = nil, nil
	d.mu.Unlock()

	if viewer != nil {
		viewer.Close(websocket.StatusNormalClosure, "server shutting down")
	}
	if deviceConn != nil {
		deviceConn.Close(websocket.StatusNormalClosure, "server shutting down")
	}
}

// Registry maps device IDs to their entries. Entries live for the process
// lifetime so the device list still shows a camera (offline, with a
// last-seen time) after it disconnects.
type Registry struct {
	devices map[string]*Device
	mu      sync.Mutex
}

func New() *Registry {
	return &Registry{
		devices: make(map[string]*Device),
	}
}

func (r *Registry) GetOrCreate(deviceID string) (*Device, error) {
	if err := ValidateDeviceID(deviceID); err != nil {
		return nil, err
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	if dev, ok := r.devices[deviceID]; ok {
		return dev, nil
	}

	dev := &Device{ID: deviceID}
	r.devices[deviceID] = dev
	log.Printf("first contact from device %s", deviceID)
	return dev, nil
}

func (r *Registry) Get(deviceID string) *Device {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.devices[deviceID]
}

func (r *Registry) All() []*Device {
	r.mu.Lock()
	defer r.mu.Unlock()

	devs := make([]*Device, 0, len(r.devices))
	for _, dev := range r.devices {
		devs = append(devs, dev)
	}
	return devs
}
