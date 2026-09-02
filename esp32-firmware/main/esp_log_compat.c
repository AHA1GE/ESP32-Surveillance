/* Compatibility shim for the esp_log() API introduced in IDF v6.
 *
 * esp_peer's prebuilt libpeer_default.a is built against an IDF whose log
 * component exports `esp_log(esp_log_config_t, tag, fmt, ...)`. IDF v5.x
 * has no such symbol, so any firmware linking esp_peer needs to provide it.
 * The config argument is a single 32-bit word (bitfields + reserved), so it
 * travels in one register - ABI-compatible with a plain int first arg.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_idf_version.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)

typedef struct {
    union {
        struct {
            uint32_t log_level : 3;
            uint32_t constrained_env : 1;
            uint32_t require_formatting : 1;
            uint32_t dis_color : 1;
            uint32_t dis_timestamp : 1;
            uint32_t binary_mode : 1;
            uint32_t reserved : 24;
        } opts;
        uint32_t data;
    };
} esp_log_config_t;

void esp_log(esp_log_config_t config, const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    esp_log_writev((esp_log_level_t)config.opts.log_level, tag, format, args);
    va_end(args);
}

#endif
