package pipeline

import (
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
)

type DeviceConfig struct {
	FFmpegPath            string
	StorageRoot           string
	HLSSegmentSeconds     int
	HLSLiveWindowSegments int
	ArchiveSegmentSeconds int
}

type Device struct {
	ID         string
	config     *DeviceConfig
	ffmpegCmd  *exec.Cmd
	stdin      io.WriteCloser
	done       chan struct{} // closed by the reap goroutine once ffmpegCmd exits
	liveDir    string
	archiveDir string
	mu         sync.Mutex
}

func NewDevice(deviceID string, cfg *DeviceConfig) (*Device, error) {
	liveDir := filepath.Join(cfg.StorageRoot, deviceID, "live")
	archiveDir := filepath.Join(cfg.StorageRoot, deviceID, "archive")

	if err := os.MkdirAll(liveDir, 0755); err != nil {
		return nil, fmt.Errorf("failed to create live directory: %w", err)
	}
	if err := os.MkdirAll(archiveDir, 0755); err != nil {
		return nil, fmt.Errorf("failed to create archive directory: %w", err)
	}

	dev := &Device{
		ID:         deviceID,
		config:     cfg,
		liveDir:    liveDir,
		archiveDir: archiveDir,
	}

	if err := dev.startFFmpeg(); err != nil {
		return nil, err
	}

	return dev, nil
}

func (d *Device) startFFmpeg() error {
	d.mu.Lock()
	defer d.mu.Unlock()

	if d.ffmpegCmd != nil {
		return nil
	}

	liveManifest := filepath.Join(d.liveDir, "index.m3u8")
	archivePattern := filepath.Join(d.archiveDir, "%Y%m%d_%H%M%S.mp4")

	// Codec options are repeated before each output: ffmpeg scopes
	// output-affecting flags to the next output URL only, so without the
	// repeat the archive output would silently fall back to the segment
	// muxer's own default codec/pix_fmt instead of matching the live one.
	args := []string{
		"-f", "image2pipe",
		"-c:v", "mjpeg",
		"-i", "pipe:0",

		"-c:v", "libx264",
		"-preset", "fast",
		"-pix_fmt", "yuv420p",
		"-f", "hls",
		"-hls_time", fmt.Sprintf("%d", d.config.HLSSegmentSeconds),
		"-hls_list_size", fmt.Sprintf("%d", d.config.HLSLiveWindowSegments),
		"-hls_flags", "delete_segments",
		liveManifest,

		"-c:v", "libx264",
		"-preset", "fast",
		"-pix_fmt", "yuv420p",
		"-f", "segment",
		"-strftime", "1",
		"-segment_time", fmt.Sprintf("%d", d.config.ArchiveSegmentSeconds),
		"-reset_timestamps", "1",
		archivePattern,
	}

	cmd := exec.Command(d.config.FFmpegPath, args...)

	stdin, err := cmd.StdinPipe()
	if err != nil {
		return fmt.Errorf("failed to get stdin pipe: %w", err)
	}

	cmd.Stdout = os.Stderr
	cmd.Stderr = os.Stderr

	if err := cmd.Start(); err != nil {
		stdin.Close()
		return fmt.Errorf("failed to start ffmpeg: %w", err)
	}

	d.ffmpegCmd = cmd
	d.stdin = stdin
	done := make(chan struct{})
	d.done = done

	// Only this goroutine ever calls Wait, so Close() doesn't race it by
	// waiting on the same *exec.Cmd a second time. It also detects crashes:
	// without a Wait() call in flight, ProcessState never populates and a
	// dead process would otherwise go unnoticed until the next failed Write.
	go func() {
		waitErr := cmd.Wait()
		d.mu.Lock()
		if d.ffmpegCmd == cmd {
			if waitErr != nil {
				log.Printf("ffmpeg for device %s exited: %v", d.ID, waitErr)
			}
			d.ffmpegCmd = nil
			d.stdin = nil
		}
		d.mu.Unlock()
		close(done)
	}()

	return nil
}

func (d *Device) WriteFrame(frame []byte) error {
	d.mu.Lock()
	defer d.mu.Unlock()

	if d.stdin == nil {
		if err := d.startFFmpeg(); err != nil {
			log.Printf("failed to restart ffmpeg for device %s: %v", d.ID, err)
			return err
		}
	}

	_, err := d.stdin.Write(frame)
	if err != nil {
		// Drop the stale handles so the next frame retries startFFmpeg
		// instead of writing into a pipe whose reader is already gone.
		d.stdin = nil
		d.ffmpegCmd = nil
	}
	return err
}

func (d *Device) Close() error {
	d.mu.Lock()
	stdin := d.stdin
	done := d.done
	d.mu.Unlock()

	if stdin != nil {
		stdin.Close()
	}
	if done != nil {
		<-done
	}

	return nil
}

func (d *Device) LiveDir() string {
	return d.liveDir
}

func (d *Device) ArchiveDir() string {
	return d.archiveDir
}
