// Package signaling runs the WebRTC control plane: devices stay connected
// on a long-lived WebSocket, viewers connect per session, and the server
// relays SDP/ICE between them while minting per-session TURN credentials.
// No media flows through the backend - frames go device -> browser over the
// DataChannel, relayed by TURN only when a direct path fails.
package signaling

import (
	"context"
	"encoding/json"
	"log"
	"net/http"
	"time"

	"github.com/AHA1GE/ESP32-Surveillance/backend/internal/registry"
	"github.com/coder/websocket"
)

// Options carries what the hub needs from server config.
type Options struct {
	STUNURLs []string      // announced to peers, e.g. ["stun:host:3478"]
	TURNURLs []string      // announced, e.g. ["turn:host:3478?transport=udp"]
	Secret   string        // TURN REST secret; empty disables TURN
	CredTTL  time.Duration // lifetime of minted credentials
}

// SignalMsg is the single JSON envelope for every message on both sockets.
// All peers ignore unknown fields, so the protocol can grow independently.
type SignalMsg struct {
	Type      string    `json:"type"`
	SDP       string    `json:"sdp,omitempty"`
	Candidate string    `json:"candidate,omitempty"`
	Reason    string    `json:"reason,omitempty"`
	STUN      []string  `json:"stun,omitempty"`
	TURN      *TurnCreds `json:"turn,omitempty"`
}

// TurnCreds is the coturn-compatible credential triple for one session.
type TurnCreds struct {
	URLs       []string `json:"urls"`
	Username   string   `json:"username"`
	Credential string   `json:"credential"`
}

// Hub relays signaling between devices and viewers and enforces the
// exactly-one-viewer rule per device.
type Hub struct {
	registry *registry.Registry
	opts     Options
}

func New(reg *registry.Registry, opts Options) *Hub {
	return &Hub{registry: reg, opts: opts}
}

// writeJSON sends one message; on failure it closes the connection so the
// owning read loop exits and runs its cleanup.
func writeJSON(ctx context.Context, conn *websocket.Conn, msg SignalMsg) error {
	data, err := json.Marshal(msg)
	if err != nil {
		return err
	}
	writeCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	if err := conn.Write(writeCtx, websocket.MessageText, data); err != nil {
		conn.Close(websocket.StatusInternalError, "write failed")
		return err
	}
	return nil
}

// ServeDeviceWS is "GET /signaling/{deviceID}": the device's long-lived
// socket. It answers offers (forwarding them to the current viewer) and
// relays the device's answers/ICE candidates to that viewer.
func (h *Hub) ServeDeviceWS(w http.ResponseWriter, r *http.Request) {
	deviceID := r.PathValue("deviceID")
	if !registry.ValidDeviceID(deviceID) {
		http.Error(w, "invalid deviceID", http.StatusBadRequest)
		return
	}

	conn, err := websocket.Accept(w, r, nil)
	if err != nil {
		log.Printf("device %s: accept failed: %v", deviceID, err)
		return
	}

	dev, err := h.registry.GetOrCreate(deviceID)
	if err != nil {
		http.Error(w, "invalid deviceID", http.StatusBadRequest)
		return
	}

	dev.Attach(conn)
	log.Printf("device %s connected", deviceID)

	// A disconnect (or a reconnect taking over the slot) kills the session:
	// the viewer is evicted so the slot frees up for the next attempt.
	defer func() {
		viewer := dev.Detach(conn)
		if viewer != nil {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			_ = writeJSON(ctx, viewer, SignalMsg{Type: "peer_gone", Reason: "device disconnected"})
			viewer.Close(websocket.StatusNormalClosure, "device disconnected")
			cancel()
		}
		conn.Close(websocket.StatusNormalClosure, "")
		log.Printf("device %s disconnected", deviceID)
	}()

	ctx := context.Background()
	for {
		msgType, data, err := conn.Read(ctx)
		if err != nil {
			return
		}
		if msgType != websocket.MessageText {
			continue
		}

		var msg SignalMsg
		if err := json.Unmarshal(data, &msg); err != nil {
			log.Printf("device %s: bad JSON: %v", deviceID, err)
			continue
		}

		switch msg.Type {
		case "answer":
			err = dev.SendViewer(ctx, mustMarshal(SignalMsg{Type: "answer", SDP: msg.SDP}))
		case "ice":
			err = dev.SendViewer(ctx, mustMarshal(SignalMsg{Type: "ice", Candidate: msg.Candidate}))
		default:
			continue // unknown types are ignored; the protocol grows forward
		}
		if err != nil {
			log.Printf("device %s: forward %s to viewer: %v", deviceID, msg.Type, err)
		}
	}
}

// ServeViewerWS is "GET /view-signaling/{deviceID}": one viewing session.
// The browser creates the offer; the device answers. The server mints TURN
// credentials, hands the ICE server list to the browser up front, and
// attaches the credentials to the offer so the device can configure its
// PeerConnection before answering.
func (h *Hub) ServeViewerWS(w http.ResponseWriter, r *http.Request) {
	deviceID := r.PathValue("deviceID")
	if !registry.ValidDeviceID(deviceID) {
		http.Error(w, "invalid deviceID", http.StatusBadRequest)
		return
	}

	conn, err := websocket.Accept(w, r, nil)
	if err != nil {
		log.Printf("viewer for %s: accept failed: %v", deviceID, err)
		return
	}

	dev := h.registry.Get(deviceID)
	if dev == nil {
		http.Error(w, "device not found", http.StatusNotFound)
		return
	}
	if !dev.Online() {
		sendError(conn, "device offline")
		conn.Close(websocket.StatusNormalClosure, "device offline")
		return
	}
	if !dev.TryClaimViewer(conn) {
		sendError(conn, "busy: another viewer is already watching")
		conn.Close(websocket.StatusPolicyViolation, "viewer slot busy")
		return
	}
	log.Printf("viewer connected for %s", deviceID)

	defer func() {
		dev.ReleaseViewer(conn)
		// Tell the device the session is over so it tears its PeerConnection
		// down and frees the heap instead of timing out on its own.
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		_ = dev.Send(ctx, mustMarshal(SignalMsg{Type: "viewer_gone"}))
		cancel()
		conn.Close(websocket.StatusNormalClosure, "")
		log.Printf("viewer disconnected from %s", deviceID)
	}()

	ctx := context.Background()

	// Session credentials: one pair shared by both peers, minted once.
	var creds *TurnCreds
	if h.opts.Secret != "" {
		c := mintTurnCreds(h.opts.Secret, deviceID, h.opts.CredTTL)
		c.URLs = h.opts.TURNURLs
		creds = &c
	}

	if err := writeJSON(ctx, conn, SignalMsg{
		Type: "ice_servers",
		STUN: h.opts.STUNURLs,
		TURN: creds,
	}); err != nil {
		return
	}

	for {
		// The browser pings every ~20s so an idle view (no ICE traffic while
		// frames flow over WebRTC) still refreshes this deadline; a viewer
		// that goes away without closing the socket is evicted within 60s.
		conn.SetReadDeadline(time.Now().Add(60 * time.Second))

		msgType, data, err := conn.Read(ctx)
		if err != nil {
			return
		}
		if msgType != websocket.MessageText {
			continue
		}

		var msg SignalMsg
		if err := json.Unmarshal(data, &msg); err != nil {
			log.Printf("viewer for %s: bad JSON: %v", deviceID, err)
			continue
		}

		switch msg.Type {
		case "offer":
			err = dev.Send(ctx, mustMarshal(SignalMsg{
				Type: "offer",
				SDP:  msg.SDP,
				STUN: h.opts.STUNURLs,
				TURN: creds,
			}))
		case "ice":
			err = dev.Send(ctx, mustMarshal(SignalMsg{Type: "ice", Candidate: msg.Candidate}))
		case "ping":
			continue // keepalive; nothing to answer
		default:
			continue
		}
		if err != nil {
			log.Printf("viewer for %s: forward %s to device: %v", deviceID, msg.Type, err)
			return
		}
	}
}

func sendError(conn *websocket.Conn, reason string) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = writeJSON(ctx, conn, SignalMsg{Type: "error", Reason: reason})
}

func mustMarshal(msg SignalMsg) []byte {
	data, err := json.Marshal(msg)
	if err != nil {
		// Only ever called with static types; a failure is a build bug.
		panic(err)
	}
	return data
}
