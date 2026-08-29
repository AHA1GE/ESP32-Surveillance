#pragma once

#include <esp_err.h>
#include <stdbool.h>

typedef struct {
    bool update_available;
    char url[512];
    char tag_name[64];
} ota_release_info_t;

esp_err_t ota_check_latest_release(const char *owner, const char *repo, ota_release_info_t *info);
esp_err_t ota_apply(const char *firmware_url);
