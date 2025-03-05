/*
 * ESP32-CAM HTTP POST Image Upload
 * This app takes jpgs 800*600 SVGA @30fps
 * and uploads them to a server via HTTP POST
 * NOTE: default cam is OV2640 (AI Thinker board pinout)
 */

#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>

// Image capture settings
#define frameHeight 600
#define frameWidth 800
#define targetFPS 30

// WiFi and Server settings
#define SSID "SSID"
#define PASSWORD "PASSWORD"
#define SERVER "192.168.1.100"
#define PORT 80
#define PATH "/upload"

// AI Thinker board pin configuration for ESP32-CAM
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27

#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

void setup()
{
    Serial.begin(115200);

    // Initialize WiFi
    WiFi.begin(SSID, PASSWORD);
    Serial.println("Connecting to WiFi...");
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // Configure camera settings
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
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000; // 20MHz clock
    config.pixel_format = PIXFORMAT_JPEG;

    // Set frame size to SVGA (800x600)
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12; // Lower number = higher quality
    config.fb_count = 1;

    // Initialize the camera
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        Serial.printf("Camera init failed with error 0x%x", err);
        return;
    }
}

void loop()
{
    // Capture a frame
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
    {
        Serial.println("Camera capture failed");
        return;
    }

    // Create HTTP POST request to send the image
    HTTPClient http;
    String url = "http://" + String(SERVER) + ":" + String(PORT) + PATH;
    http.begin(url);
    http.addHeader("Content-Type", "image/jpeg");

    int httpResponseCode = http.POST(fb->buf, fb->len);
    if (httpResponseCode > 0)
    {
        Serial.printf("HTTP Response code: %d\n", httpResponseCode);
    }
    else
    {
        Serial.printf("Error sending POST: %s\n", http.errorToString(httpResponseCode).c_str());
    }
    http.end();

    // Return the frame buffer for reuse
    esp_camera_fb_return(fb);

    // Delay to approximate targetFPS (30fps = ~33ms per frame)
    delay(1000 / targetFPS);
}
