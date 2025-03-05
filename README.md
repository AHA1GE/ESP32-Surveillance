# ESP32 Surveillance Platform

A surveillance platform using one or more ESP32-CAM modules (with OV2640) and a backend server to capture, process, and generate video clips from live image streams.

## Overview

This project aims to create a scalable surveillance system with the following capabilities:

- **ESP32-CAM Module:** Capture JPEG images at a configurable resolution (e.g., 800×600) and frame rate (target 30fps).
- **Backend Server:** A Python-based server that receives images from the ESP32 devices, stores them, and automatically generates a video clip (e.g., 10 seconds of video at 30fps) once the required number of images is received.
- **Scalability:** Designed to support multiple ESP32-CAM devices with future transition to a serverless architecture and cloud-based storage/databases.

## Features

- **ESP32 Firmware:**

  - Developed using VS Code with the Arduino extension.
  - Configurable image capture (resolution and fps).
  - Upload images via HTTP/HTTPS to a backend server.
  - Includes basic error handling and retry mechanisms.

- **Backend Server (Prototype):**

  - Python-based REST API for receiving image uploads.
  - Temporary storage of images organized by device and timestamp.
  - Video generation using libraries such as OpenCV or FFmpeg.
  - Multi-device support with tagging (device IDs).

- **Future Enhancements:**
  - Transition to serverless functions (e.g., AWS Lambda, Google Cloud Functions).
  - Integration with cloud storage (e.g., AWS S3) and cloud databases.
  - Improved security and network resiliency features.
  - Real-time monitoring and logging for performance optimization.

## Getting Started

### Prerequisites

- **Hardware:**

  - ESP32-CAM module with OV2640 camera.
  - Backend PC/server for development and testing.

- **Software:**
  - [Visual Studio Code](https://code.visualstudio.com/) with the Arduino plugin.
  - Arduino IDE (if preferred) for ESP32 development.
  - Python 3.x and necessary libraries (e.g., Flask/FastAPI, OpenCV, or FFmpeg bindings).

### Setup

1. **ESP32 Firmware:**

   - Clone the repository and open the project in VS Code.
   - Configure your ESP32-CAM settings (WiFi credentials, target resolution, fps, and backend URL) in the firmware code.
   - Upload the firmware to your ESP32-CAM module.

2. **Python Backend:**

   - Navigate to the backend directory.
   - Install required Python dependencies using:
     ```bash
     pip install -r requirements.txt
     ```
   - Run the server locally:
     ```bash
     python app.py
     ```
   - Ensure the API endpoint (e.g., `/upload`) is reachable and properly receives image data.

3. **Testing:**
   - Use your ESP32-CAM module to start sending JPEG images to the backend.
   - Monitor the backend logs and verify that images are stored.
   - Once the configured number of images (e.g., 300 for 10 seconds at 30fps) is received, the video generation process should be triggered.

## Roadmap

see [ROADMAP.md](ROADMAP.md)

## Contributing

Contributions and feedback are welcome. Please open an issue or submit a pull request with your suggestions.

## License

This project is licensed under the MIT License.
