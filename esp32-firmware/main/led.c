#include "led.h"

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#define TAG "led"

/* Onboard red LED. Active LOW: level 0 turns it ON (driver transistor on the
 * board inverts the logic). Plain GPIO - the camera owns LEDC_TIMER_0 /
 * LEDC_CHANNEL_0 for XCLK, so this must never touch LEDC. */
#define LED_GPIO GPIO_NUM_33

#define LED_QUEUE_LEN 4

typedef struct {
    led_pattern_t pattern;     /* new persistent pattern (ignored for transients) */
    uint8_t transient_flashes; /* >0: run N fast flashes, then restore previous */
} led_cmd_t;

static QueueHandle_t s_led_queue;

static void led_set_level(int level)
{
    gpio_set_level(LED_GPIO, level);
}

static void led_task(void *arg)
{
    led_cmd_t cmd;
    led_pattern_t current = LED_PATTERN_OFF;
    led_pattern_t restore = LED_PATTERN_OFF;
    uint8_t transient_remaining = 0;

    while (1) {
        /* Drain queued commands first so pattern changes apply promptly. */
        while (xQueueReceive(s_led_queue, &cmd, 0) == pdTRUE) {
            if (cmd.transient_flashes > 0) {
                transient_remaining = cmd.transient_flashes;
                restore = current;
            } else {
                transient_remaining = 0;
                current = cmd.pattern;
            }
        }

        led_pattern_t active = (transient_remaining > 0) ? LED_PATTERN_FAST_FLASH : current;

        switch (active) {
        case LED_PATTERN_OFF:
            led_set_level(1);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        case LED_PATTERN_SOLID:
            led_set_level(0);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        case LED_PATTERN_DOUBLE_BLINK:
            /* two quick blinks, then a long pause (period 800 ms) */
            led_set_level(0); vTaskDelay(pdMS_TO_TICKS(80));
            led_set_level(1); vTaskDelay(pdMS_TO_TICKS(80));
            led_set_level(0); vTaskDelay(pdMS_TO_TICKS(80));
            led_set_level(1); vTaskDelay(pdMS_TO_TICKS(560));
            break;
        case LED_PATTERN_FAST_FLASH:
            led_set_level(0); vTaskDelay(pdMS_TO_TICKS(110));
            led_set_level(1); vTaskDelay(pdMS_TO_TICKS(110));
            break;
        }

        if (active == LED_PATTERN_FAST_FLASH && transient_remaining > 0) {
            transient_remaining--;
            if (transient_remaining == 0) {
                current = restore;
            }
        }
    }
}

esp_err_t led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    led_set_level(1); /* active low: off */

    s_led_queue = xQueueCreate(LED_QUEUE_LEN, sizeof(led_cmd_t));
    if (!s_led_queue) {
        return ESP_ERR_NO_MEM;
    }

    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
    return ESP_OK;
}

void led_set_pattern(led_pattern_t pattern)
{
    if (!s_led_queue) {
        return;
    }
    led_cmd_t cmd = { .pattern = pattern, .transient_flashes = 0 };
    xQueueSend(s_led_queue, &cmd, 0);
}

void led_error_flash(uint8_t flashes)
{
    if (!s_led_queue) {
        return;
    }
    led_cmd_t cmd = { .pattern = LED_PATTERN_FAST_FLASH, .transient_flashes = flashes };
    xQueueSend(s_led_queue, &cmd, 0);
}
