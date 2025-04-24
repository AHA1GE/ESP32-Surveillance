/*
 * ESP32-CAM HTTP POST Image Upload
 * This app takes jpgs 800*600 SVGA @30fps
 * and uploads them to a server via HTTP POST
 * NOTE: default cam is OV2640 (AI Thinker board pinout)
 */

#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <ArduinoHttpClient.h>
// ===================
// Select camera model
// ===================
// #define CAMERA_MODEL_WROVER_KIT // Has PSRAM
// #define CAMERA_MODEL_ESP_EYE  // Has PSRAM
// #define CAMERA_MODEL_ESP32S3_EYE // Has PSRAM
// #define CAMERA_MODEL_M5STACK_PSRAM // Has PSRAM
// #define CAMERA_MODEL_M5STACK_V2_PSRAM // M5Camera version B Has PSRAM
// #define CAMERA_MODEL_M5STACK_WIDE // Has PSRAM
// #define CAMERA_MODEL_M5STACK_ESP32CAM // No PSRAM
// #define CAMERA_MODEL_M5STACK_UNITCAM // No PSRAM
// #define CAMERA_MODEL_M5STACK_CAMS3_UNIT  // Has PSRAM
#define CAMERA_MODEL_AI_THINKER // Has PSRAM
// #define CAMERA_MODEL_TTGO_T_JOURNAL // No PSRAM
// #define CAMERA_MODEL_XIAO_ESP32S3 // Has PSRAM
//  ** Espressif Internal Boards **
// #define CAMERA_MODEL_ESP32_CAM_BOARD
// #define CAMERA_MODEL_ESP32S2_CAM_BOARD
// #define CAMERA_MODEL_ESP32S3_CAM_LCD
// #define CAMERA_MODEL_DFRobot_FireBeetle2_ESP32S3 // Has PSRAM
// #define CAMERA_MODEL_DFRobot_Romeo_ESP32S3 // Has PSRAM
#include "camera_pins.h"

// Image capture settings
#define frameHeight 600
#define frameWidth 800
#define targetFPS 30

#define wsServerAddress "192.168.1.102"
#define wsServerPort 8081
#define wsServerPath "/upload"

const char *WIFI_LIST[3][2] = {
    {"TEST", "00000000"},
    {"ZT", "ztMODEL168!#"},
    {"FW_FAST", "ff702543702"},
};

WiFiMulti wifiMulti;
WiFiClient wifiClientInstance;
WebSocketClient wsClient = WebSocketClient(wifiClientInstance, wsServerAddress, wsServerPort); // Declare WebSocket client instance

bool testConnection()
{
  HTTPClient http;
  http.begin("http://captive.apple.com");
  int httpCode = http.GET();
  // expect to get 200 (OK) or 301 (https upgrade) response code
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY)
  {
    return true;
  }
  return false;
}

void initWiFi()
{
  for (int i = 0; i < 3; i++)
  {
    wifiMulti.addAP(WIFI_LIST[i][0], WIFI_LIST[i][1]);
  }
  wifiMulti.enableIPv6(true);
  wifiMulti.setStrictMode(false);                          // Default is true.  Library will disconnect and forget currently connected AP if it's not in the AP list.
  wifiMulti.setAllowOpenAP(true);                          // Default is false.  True adds open APs to the AP list.
  wifiMulti.setConnectionTestCallbackFunc(testConnection); // Attempts to connect to a remote webserver in case of captive portals.

  Serial.println("Wi-Fi Connecting...");
}

bool initCAM()
{
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG; // for streaming
  // config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  if (config.pixel_format == PIXFORMAT_JPEG)
  {
    if (psramFound())
    {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    }
    else
    {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  }
  else
  {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
  }
  esp_err_t err = esp_camera_init(&config); // Initialize the camera
  if (err == ESP_OK)
  {
    return true;
  }
  else
  {
    Serial.printf("Camera init failed with error 0x%x. ", err);
    return false;
  }
}

void blinkLed(int times, int onDuration = 50, int offDuration = 200)
{
  for (int i = 0; i < times; i++)
  {
    digitalWrite(LED_GPIO_NUM, HIGH); // Turn the LED on
    delay(onDuration);                // Wait for a while
    digitalWrite(LED_GPIO_NUM, LOW);  // Turn the LED off
    delay(offDuration);               // Wait for a while
  }
}

void setup()
{
  Serial.begin(115200);
  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);
  // Wi-Fi
  initWiFi();
  static bool isConnected = false;
  while (!isConnected)
  {
    uint8_t WiFiStatus = wifiMulti.run(5000, true); // 10s per ssid, scan hidden = true
    if (WiFiStatus == WL_CONNECTED)
    {
      isConnected = true;
      Serial.printf("Wi-Fi Connected:\n  SSID:\t%s\n  PASS:\t%s\n  IP:\t%s\n", WiFi.SSID().c_str(), WiFi.psk().c_str(), WiFi.localIP().toString().c_str());
      blinkLed(1, 50, 450);
    }
  }
  // CAM
  static bool isCamReady = false;
  while (!isCamReady)
  {
    isCamReady = initCAM(); // Initialize the camera
    if (!isCamReady)
    {
      Serial.println("Retry in 5s");
      delay(5000); // Wait before retrying
    }
    else
    {
      Serial.println("Camera ready.");
      blinkLed(2);
    }
  }
  // WebSocket
  wsClient.begin(wsServerPath);
  static bool isWsConnected = false;
  while (!isWsConnected)
  {
    if (wsClient.ping() == 0)
    {
      Serial.println("WebSocket ping sent.");
      // ping sent, wait for pong
      char buffer[128];
      int len = wsClient.readBytes(buffer, sizeof(buffer) - 1);
      if (len > 0)
      {
        buffer[len] = '\0'; // Null-terminate the string
        Serial.printf("WebSocket connected: %s\n", buffer);
        isWsConnected = true;
        blinkLed(3);
      }
      else
      {
        Serial.println("WebSocket connection failed. Retry in 5s.");
        delay(5000); // Wait before retrying
      }
    }
    else
    {
      Serial.println("WebSocket send ping failed. Retry in 5s.");
      delay(5000); // Wait before retrying
    }
  }
}

void loop()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    camera_fb_t *fb = esp_camera_fb_get(); // Capture a frame
    if (!fb)
    {
      Serial.println("Camera capture failed");
      delay(100); // small delay before retrying
      return;
    }

    wsClient.beginMessage(TYPE_BINARY);
    wsClient.write(fb->buf, fb->len); // Send the image data
    wsClient.endMessage();
    // Serial.println("fb sent on ws.");

    esp_camera_fb_return(fb); // Return the frame buffer for reuse

    // delay(1000 / targetFPS); // Uncomment and adjust if specific FPS is desired
  }
  else
  {
    Serial.println("WiFi disconnected. Rebooting...");
    esp_camera_deinit(); // Deinitialize camera before rebooting
    ESP.restart();       // software reset
  }
}
