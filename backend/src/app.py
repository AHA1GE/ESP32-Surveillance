"""
ESP32 Surveillance Backend

This Flask application serves as the backend for an ESP32-based surveillance system.
It handles the following functionalities:
- Receiving and saving image frames uploaded from the ESP32 device.
- Encoding the collected frames into a video using ffmpeg once a sufficient number of frames are collected.
- Providing a RESTful API endpoint for uploading image frames.

Configuration parameters:
- TARGET_FPS: Frames per second for the output video.
- TARGET_BITRATE: Bitrate for the output video.
- TARGET_RESOLUTION: Resolution for the output video.
- TARGET_CODEC: Codec to be used for encoding the video.
- TARGET_VIDEO_LENGTH: Length of the output video in seconds.
- FRAMES_NEEDED: Total number of frames needed to encode the video.

Directories:
- TMP_DIR: Temporary directory to store uploaded image frames.
- VIDEOS_DIR: Directory to store the encoded videos.

Endpoints:
- /upload: POST endpoint to upload image frames.

To run the application:
- Ensure you have Flask and ffmpeg installed.
- Run the script using `python app.py`.

Author: [Your Name]
Date: [Date]
"""

import os
import glob
import subprocess
from datetime import datetime
from flask import Flask, request, jsonify

app = Flask(__name__)

# Configuration parameters
TARGET_FPS = 30
TARGET_BITRATE = "1Mbps"
TARGET_RESOLUTION = "800x600"
TARGET_CODEC = "libx265"
TARGET_VIDEO_LENGTH = 30  # seconds
FRAMES_NEEDED = TARGET_FPS * TARGET_VIDEO_LENGTH

TMP_DIR = '/tmp'
VIDEOS_DIR = '/videos'

# Ensure the videos directory exists
os.makedirs(VIDEOS_DIR, exist_ok=True)

def encode_video():
    # Get list of jpg files sorted by filename (assumes filenames are time-based)
    frame_files = sorted(glob.glob(os.path.join(TMP_DIR, '*.jpg')))
    if len(frame_files) < FRAMES_NEEDED:
        print("Not enough frames to encode video.")
        return False

    # Build output filename with current timestamp
    timestamp = datetime.now().strftime("%Y%m%d%H%M%S")
    output_file = os.path.join(VIDEOS_DIR, f"video_{timestamp}.mp4")
    
    # Build the ffmpeg command
    # Using glob pattern to capture all jpg files in /tmp/
    ffmpeg_cmd = [
        'ffmpeg',
        '-y',  # Overwrite output file if it exists
        '-framerate', str(TARGET_FPS),
        '-pattern_type', 'glob',
        '-i', os.path.join(TMP_DIR, '*.jpg'),
        '-c:v', TARGET_CODEC,
        '-b:v', TARGET_BITRATE,
        '-s', TARGET_RESOLUTION,
        output_file
    ]
    
    print("Running ffmpeg command:", " ".join(ffmpeg_cmd))
    result = subprocess.run(ffmpeg_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        print("ffmpeg error:", result.stderr.decode())
        return False
    else:
        print("Video encoded successfully:", output_file)
        # Delete JPEG frames after successful encoding
        for file in frame_files:
            os.remove(file)
        return True

@app.route('/upload', methods=['POST'])
def upload():
    # Ensure a file is provided in the request
    if 'file' not in request.files:
        return jsonify({'error': 'No file part in the request'}), 400
    
    file = request.files['file']
    if file.filename == '':
        return jsonify({'error': 'No selected file'}), 400

    # Save the file with a timestamp-based filename to /tmp/
    timestamp = datetime.now().strftime("%Y%m%d%H%M%S%f")
    filename = f"{timestamp}.jpg"
    file_path = os.path.join(TMP_DIR, filename)
    file.save(file_path)
    print(f"Saved file: {file_path}")

    # Check if enough frames are collected to encode a video
    jpg_files = glob.glob(os.path.join(TMP_DIR, '*.jpg'))
    if len(jpg_files) >= FRAMES_NEEDED:
        print(f"Collected {len(jpg_files)} frames. Starting video encoding.")
        encode_video()

    # Return the filename as confirmation to the client
    return jsonify({'filename': filename})

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)
