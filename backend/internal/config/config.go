package config

import (
	"os"
	"strconv"
	"time"
)

type Config struct {
	ListenAddr     string
	UIListenAddr   string
	TURNListenAddr string
	TURNPublicAddr string // announced to peers; IP:port. Empty disables TURN.
	TURNSecret     string // empty disables TURN
	TURNCredHours  int
}

func Load() *Config {
	return &Config{
		ListenAddr:     getEnv("LISTEN_ADDR", ":8080"),
		UIListenAddr:   getEnv("UI_LISTEN_ADDR", ""),
		TURNListenAddr: getEnv("TURN_LISTEN_ADDR", ":3478"),
		TURNPublicAddr: getEnv("TURN_PUBLIC_ADDR", ""),
		TURNSecret:     getEnv("TURN_SECRET", ""),
		TURNCredHours:  getEnvInt("TURN_CRED_HOURS", 2),
	}
}

// TURNEnabled reports whether both TURN settings are present. A secret
// without a public address (or vice versa) is a misconfiguration, but the
// server still starts: peers just get no ICE servers and fall back to
// direct host candidates on the LAN.
func (c *Config) TURNEnabled() bool {
	return c.TURNSecret != "" && c.TURNPublicAddr != ""
}

func (c *Config) TURNCredTTL() time.Duration {
	return time.Duration(c.TURNCredHours) * time.Hour
}

func getEnv(key, defaultVal string) string {
	if val, ok := os.LookupEnv(key); ok {
		return val
	}
	return defaultVal
}

func getEnvInt(key string, defaultVal int) int {
	if val, ok := os.LookupEnv(key); ok {
		if i, err := strconv.Atoi(val); err == nil {
			return i
		}
	}
	return defaultVal
}
