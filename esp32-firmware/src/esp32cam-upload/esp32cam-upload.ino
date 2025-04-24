/*
 * ESP32-CAM HTTP POST Image Upload
 * This app takes jpgs 800*600 SVGA @30fps
 * and uploads them to a server via HTTP POST
 * NOTE: default cam is OV2640 (AI Thinker board pinout)
 */

#include <Arduino.h>
// #include "esp_camera.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <ArduinoHttpClient.h>
#include "cam-config.h"

// Image capture settings
#define frameHeight 600
#define frameWidth 800
#define targetFPS 30

#define wsServerAddress "192.168.1.100"
#define wsServerPort 8081
#define wsServerPath "/upload"

const char *WIFI_LIST[3][2] = {
  { "TEST", "00000000" },
  { "ZT", "ztMODEL168!#" },
  { "FW_FAST", "ff702543702" },
};

WiFiMulti wifiMulti;
WiFiClient wifiClientInstance;
WebSocketClient wsClient = WebSocketClient(wifiClientInstance, wsServerAddress, wsServerPort);  // Declare WebSocket client instance

bool testConnection() {
  HTTPClient http;
  http.begin("http://captive.apple.com");
  int httpCode = http.GET();
  // expect to get 200 (OK) or 301 (https upgrade) response code
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
    return true;
  }
  return false;
}

void initWiFi() {
  for (int i = 0; i < 3; i++) {
    wifiMulti.addAP(WIFI_LIST[i][0], WIFI_LIST[i][1]);
  }
  wifiMulti.setStrictMode(false);                           // Default is true.  Library will disconnect and forget currently connected AP if it's not in the AP list.
  wifiMulti.setAllowOpenAP(true);                           // Default is false.  True adds open APs to the AP list.
  wifiMulti.setConnectionTestCallbackFunc(testConnection);  // Attempts to connect to a remote webserver in case of captive portals.
  Serial.print("Connecting Wifi.");
}

void setup() {
  Serial.begin(115200);
  initWiFi();
  static bool isConnected = false;
  while (!isConnected) {
    uint8_t WiFiStatus = wifiMulti.run(5000, true);  // 10s per ssid, scan hidden = true
    if (WiFiStatus == WL_CONNECTED) {
      isConnected = true;
      Serial.println("Connected to WiFi.");
    } 
  }
  Serial.printf("ESP32 IP Address: %s\n", WiFi.localIP().toString().c_str());

  // Configure camera settings
  extern const camera_config_t config;

  // Initialize the camera
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    // Consider restarting or halting
    ESP.restart();
    return;
  }
  Serial.println("Camera initialized successfully.");

  // Start WebSocket connection
  wsClient.begin(wsServerPath);
  Serial.println("WebSocket connection initiated.");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    camera_fb_t *fb = esp_camera_fb_get();  // Capture a frame
    if (!fb) {
      Serial.println("Camera capture failed");
      delay(100);  // small delay before retrying
      return;
    }

    wsClient.beginMessage(TYPE_BINARY);
    wsClient.write(fb->buf, fb->len);  // Send the image data
    wsClient.endMessage();

    esp_camera_fb_return(fb);  // Return the frame buffer for reuse

    // delay(1000 / targetFPS); // Uncomment and adjust if specific FPS is desired
  } else {
    Serial.println("WiFi disconnected. Rebooting...");
    esp_camera_deinit();  // Deinitialize camera before rebooting
    ESP.restart();        // software reset
  }
}
