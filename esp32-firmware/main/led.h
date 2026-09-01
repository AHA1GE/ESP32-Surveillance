#pragma once

#include <esp_err.h>
#include <stdint.h>

typedef enum {
    LED_PATTERN_OFF = 0,      /* LED dark */
    LED_PATTERN_SOLID,        /* streaming to a connected backend */
    LED_PATTERN_DOUBLE_BLINK, /* config portal mode */
    LED_PATTERN_FAST_FLASH,   /* error: camera fail, backend down, invalid submit */
} led_pattern_t;

/** Configures GPIO33 as output (LED off) and starts the pattern task. */
esp_err_t led_init(void);

/** Persistent pattern change (returns immediately, applied by the LED task). */
void led_set_pattern(led_pattern_t pattern);

/** Transient: `flashes` fast flashes, then restore the previous pattern. */
void led_error_flash(uint8_t flashes);
