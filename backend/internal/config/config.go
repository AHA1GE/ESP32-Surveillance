package config

import (
	"os"
	"strconv"
	"time"
)

type Config struct {
	ListenAddr              string
	UIListenAddr            string
	StorageRoot             string
	FFmpegPath              string
	HLSSegmentSeconds       int
	HLSLiveWindowSegments   int
	ArchiveSegmentSeconds   int
	ArchiveRetentionDays    int
	RetentionCheckInterval  time.Duration
}

func Load() *Config {
	return &Config{
		ListenAddr:            getEnv("LISTEN_ADDR", ":8080"),
		UIListenAddr:          getEnv("UI_LISTEN_ADDR", ""),
		StorageRoot:           getEnv("STORAGE_ROOT", "./storage"),
		FFmpegPath:            getEnv("FFMPEG_PATH", "ffmpeg"),
		HLSSegmentSeconds:     getEnvInt("HLS_SEGMENT_SECONDS", 4),
		HLSLiveWindowSegments: getEnvInt("HLS_LIVE_WINDOW_SEGMENTS", 10),
		ArchiveSegmentSeconds: getEnvInt("ARCHIVE_SEGMENT_SECONDS", 300),
		ArchiveRetentionDays:  getEnvInt("ARCHIVE_RETENTION_DAYS", 7),
		RetentionCheckInterval: time.Duration(getEnvInt("RETENTION_CHECK_MINUTES", 5)) * time.Minute,
	}
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
