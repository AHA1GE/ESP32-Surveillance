package signaling

import (
	"crypto/hmac"
	"crypto/sha1"
	"encoding/base64"
	"fmt"
	"log"
	"net"
	"strconv"
	"time"

	"github.com/pion/turn/v4"
)

// TURNOptions configures the embedded TURN server. PublicAddr is the
// address peers use to reach the relay - it must be an IP:port (resolve
// hostnames before starting), because it becomes the relay address inside
// TURN allocations.
type TURNOptions struct {
	ListenAddr string // local bind address, e.g. ":3478"
	PublicAddr string // announced address, e.g. "203.0.113.7:3478"
	Secret     string
	Realm      string
}

const (
	turnRelayPortMin = 50000
	turnRelayPortMax = 55000
)

// leveledLogger adapts the standard logger to pion/logging's interface so
// TURN auth failures land in the same log stream as everything else.
type leveledLogger struct{ logger *log.Logger }

func (l *leveledLogger) logf(format string, args ...interface{}) {
	l.logger.Printf("TURN: "+format, args...)
}
func (l *leveledLogger) Trace(msg string)                          {}
func (l *leveledLogger) Tracef(format string, args ...interface{}) {}
func (l *leveledLogger) Debug(msg string)                          {}
func (l *leveledLogger) Debugf(format string, args ...interface{}) {}
func (l *leveledLogger) Info(msg string)                           { l.logf("%s", msg) }
func (l *leveledLogger) Infof(format string, args ...interface{})  { l.logf(format, args...) }
func (l *leveledLogger) Warn(msg string)                           { l.logf("%s", msg) }
func (l *leveledLogger) Warnf(format string, args ...interface{})  { l.logf(format, args...) }
func (l *leveledLogger) Error(msg string)                          { l.logf("%s", msg) }
func (l *leveledLogger) Errorf(format string, args ...interface{}) { l.logf(format, args...) }

// StartTURN runs the pion/turn server (which also answers STUN binding
// requests on the same UDP port - that is the backend's STUN server). Call
// Close on the returned server to stop it. TCP and UDP share the port so
// restrictive networks can fall back to TCP candidates.
func StartTURN(opts TURNOptions, logger *log.Logger) (*turn.Server, error) {
	udpListener, err := net.ListenPacket("udp4", opts.ListenAddr)
	if err != nil {
		return nil, fmt.Errorf("TURN udp listen on %s: %w", opts.ListenAddr, err)
	}

	tcpListener, err := net.Listen("tcp4", opts.ListenAddr)
	if err != nil {
		udpListener.Close()
		return nil, fmt.Errorf("TURN tcp listen on %s: %w", opts.ListenAddr, err)
	}

	host, portStr, err := net.SplitHostPort(opts.PublicAddr)
	if err != nil {
		udpListener.Close()
		tcpListener.Close()
		return nil, fmt.Errorf("TURN_PUBLIC_ADDR %q: %w", opts.PublicAddr, err)
	}
	relayIP := net.ParseIP(host)
	if relayIP == nil {
		udpListener.Close()
		tcpListener.Close()
		return nil, fmt.Errorf("TURN_PUBLIC_ADDR %q: host must be an IP, not a hostname", opts.PublicAddr)
	}
	if _, err := strconv.Atoi(portStr); err != nil {
		udpListener.Close()
		tcpListener.Close()
		return nil, fmt.Errorf("TURN_PUBLIC_ADDR %q: %w", opts.PublicAddr, err)
	}

	// AuthHandler is the v4 signature: returns (key []byte, ok bool), and
	// the REST handler validates the "expiry:label" username format itself,
	// so per-session credentials never need a long-lived password store.
	server, err := turn.NewServer(turn.ServerConfig{
		Realm:       opts.Realm,
		AuthHandler: turn.LongTermTURNRESTAuthHandler(opts.Secret, &leveledLogger{logger}),
		PacketConnConfigs: []turn.PacketConnConfig{
			{
				PacketConn: udpListener,
				// Net is left nil: Validate() fills in a default stdnet that
				// picks UDP vs TCP by the method the server calls on it.
				RelayAddressGenerator: &turn.RelayAddressGeneratorPortRange{
					RelayAddress: relayIP,
					Address:      host,
					MinPort:      turnRelayPortMin,
					MaxPort:      turnRelayPortMax,
				},
			},
		},
		ListenerConfigs: []turn.ListenerConfig{
			{
				Listener: tcpListener,
				RelayAddressGenerator: &turn.RelayAddressGeneratorPortRange{
					RelayAddress: relayIP,
					Address:      host,
					MinPort:      turnRelayPortMin,
					MaxPort:      turnRelayPortMax,
				},
			},
		},
	})
	if err != nil {
		udpListener.Close()
		tcpListener.Close()
		return nil, fmt.Errorf("start TURN server: %w", err)
	}

	logger.Printf("TURN listening on %s (announced %s, realm %s)", opts.ListenAddr, opts.PublicAddr, opts.Realm)
	return server, nil
}

// mintTurnCreds builds a coturn-compatible time-limited credential pair:
// username "unixExpiry:label", password base64(HMAC-SHA1(secret, username)).
// The same pair is handed to both peers for one viewing session, so no
// long-lived secret ever reaches a device or a browser.
func mintTurnCreds(secret, label string, ttl time.Duration) TurnCreds {
	expiry := time.Now().Add(ttl).Unix()
	username := fmt.Sprintf("%d:%s", expiry, label)

	mac := hmac.New(sha1.New, []byte(secret))
	mac.Write([]byte(username))
	credential := base64.StdEncoding.EncodeToString(mac.Sum(nil))

	return TurnCreds{
		Username:   username,
		Credential: credential,
	}
}
