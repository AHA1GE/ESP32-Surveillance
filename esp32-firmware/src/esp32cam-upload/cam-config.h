#ifndef CAM_CONFIG_H
#define CAM_CONFIG_H

#include "esp_camera.h"

// AI Thinker board pin configuration for ESP32-CAM
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET 32
#define CAM_PIN_XCLK 0
#define CAM_PIN_SDA 26
#define CAM_PIN_SCL 27
#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22

// typedef struct {
//     int pin_pwdn;                   /*!< GPIO pin for camera power down line */
//     int pin_reset;                  /*!< GPIO pin for camera reset line */
//     int pin_xclk;                   /*!< GPIO pin for camera XCLK line */
//     union {
//         int pin_sccb_sda;           /*!< GPIO pin for camera SDA line */
//         int pin_sscb_sda __attribute__((deprecated("please use pin_sccb_sda instead")));           /*!< GPIO pin for camera SDA line (legacy name) */
//     };
//     union {
//         int pin_sccb_scl;           /*!< GPIO pin for camera SCL line */
//         int pin_sscb_scl __attribute__((deprecated("please use pin_sccb_scl instead")));           /*!< GPIO pin for camera SCL line (legacy name) */
//     };
//     int pin_d7;                     /*!< GPIO pin for camera D7 line */
//     int pin_d6;                     /*!< GPIO pin for camera D6 line */
//     int pin_d5;                     /*!< GPIO pin for camera D5 line */
//     int pin_d4;                     /*!< GPIO pin for camera D4 line */
//     int pin_d3;                     /*!< GPIO pin for camera D3 line */
//     int pin_d2;                     /*!< GPIO pin for camera D2 line */
//     int pin_d1;                     /*!< GPIO pin for camera D1 line */
//     int pin_d0;                     /*!< GPIO pin for camera D0 line */
//     int pin_vsync;                  /*!< GPIO pin for camera VSYNC line */
//     int pin_href;                   /*!< GPIO pin for camera HREF line */
//     int pin_pclk;                   /*!< GPIO pin for camera PCLK line */

//     int xclk_freq_hz;               /*!< Frequency of XCLK signal, in Hz. EXPERIMENTAL: Set to 16MHz on ESP32-S2 or ESP32-S3 to enable EDMA mode */

//     ledc_timer_t ledc_timer;        /*!< LEDC timer to be used for generating XCLK  */
//     ledc_channel_t ledc_channel;    /*!< LEDC channel to be used for generating XCLK  */

//     pixformat_t pixel_format;       /*!< Format of the pixel data: PIXFORMAT_ + YUV422|GRAYSCALE|RGB565|JPEG  */
//     framesize_t frame_size;         /*!< Size of the output image: FRAMESIZE_ + QVGA|CIF|VGA|SVGA|XGA|SXGA|UXGA  */

//     int jpeg_quality;               /*!< Quality of JPEG output. 0-63 lower means higher quality  */
//     size_t fb_count;                /*!< Number of frame buffers to be allocated. If more than one, then each frame will be acquired (double speed)  */
//     camera_fb_location_t fb_location; /*!< The location where the frame buffer will be allocated */
//     camera_grab_mode_t grab_mode;   /*!< When buffers should be filled */
// #if CONFIG_CAMERA_CONVERTER_ENABLED
//     camera_conv_mode_t conv_mode;   /*!< RGB<->YUV Conversion mode */
// #endif

//     int sccb_i2c_port;              /*!< If pin_sccb_sda is -1, use the already configured I2C bus by number */
// } camera_config_t;

static const camera_config_t config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SDA,
    .pin_sccb_scl = CAM_PIN_SCL,
    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,
    .xclk_freq_hz = 20000000,       // Frequency of XCLK signal, in Hz
    .ledc_timer = LEDC_TIMER_0,     // LEDC timer to be used for generating XCLK
    .ledc_channel = LEDC_CHANNEL_0, // LEDC channel to be used for generating XCLK
    .pixel_format = PIXFORMAT_JPEG, // Format of the pixel data
    .frame_size = FRAMESIZE_SVGA,   // Size of the output image
    .jpeg_quality = 12,             // Quality of JPEG output
    .fb_count = 1                   // Number of frame buffers to be allocated
};

#endif // CAM_CONFIG_H