# Roadmap

This document outlines the planned development phases and key milestones for the ESP32 Surveillance Platform.

## Phase 1: Prototype Development

- **ESP32 Firmware:**

  - [x] Set up development environment (VS Code with Arduino plugin).
  - [x] Implement basic image capture using the OV2640.
  - [x] Configure image capture parameters (resolution, fps).
  - [x] Develop network communication to upload images to the backend.
  - [ ] Integrate error handling and retry mechanisms for unstable network conditions.

- **Python Backend (Prototype):**
  - [x] Set up a basic RESTful API endpoint (e.g., `/upload`) to receive JPEG images.
  - [x] Store received images in an organized directory structure (by device and timestamp).
  - [ ] Implement a check to detect when the configured number of images has been reached.
  - [ ] Integrate video generation using OpenCV or FFmpeg once enough images are collected.
  - [ ] Clean up temporary storage (delete images post video generation).

## Phase 2: Optimization & Multi-Device Support

- **Performance Tuning:**

  - [ ] Test and optimize the performance of the ESP32-CAM (adjust resolution/fps as needed).
  - [ ] Improve network throughput and reliability.
  - [ ] Optimize file I/O and image processing in the backend.

- **Multi-Device Handling:**
  - [ ] Implement device identification in image uploads.
  - [ ] Ensure concurrent image processing and video generation for multiple devices.
  - [ ] Develop monitoring tools to track performance and system health.

## Phase 3: Transition to Cloud & Serverless Architecture

- **Cloud Migration:**

  - [ ] Transition the image receiving API to serverless functions (AWS Lambda, Azure Functions, etc.).
  - [ ] Move temporary storage to a cloud storage solution (e.g., AWS S3).
  - [ ] Integrate a cloud-based database to track image metadata and video generation status.

- **Serverless Video Generation:**
  - [ ] Develop cloud-based processing functions for video creation.
  - [ ] Implement triggering mechanisms (e.g., using message queues) to start video generation once image thresholds are met.
  - [ ] Automate cleanup of raw images post video generation.

## Phase 4: Security & Reliability Enhancements

- **Security Improvements:**

  - [ ] Secure communications between ESP32 devices and the backend (e.g., implement TLS).
  - [ ] Add authentication mechanisms for API endpoints to prevent unauthorized uploads.

- **Reliability & Monitoring:**
  - [ ] Integrate logging and monitoring solutions for both hardware and backend services.
  - [ ] Set up automated alerts for system failures or performance issues.
  - [ ] Regularly review and update the system based on user feedback and performance metrics.
