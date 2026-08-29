#pragma once

#include <esp_err.h>
#include <esp_camera.h>

esp_err_t camera_init(void);
camera_fb_t *camera_capture(void);
void camera_release(camera_fb_t *fb);
