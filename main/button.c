#include "button.h"

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if CONFIG_LAMP_INPUT_TOUCH
#include "touch.h"
#endif

#define POLL_MS         10
#define DEBOUNCE_COUNT  3 /* consecutive samples needed to accept a change */

static const char *TAG = "button";

static button_callback_t s_cb;
static volatile int64_t s_inject_until_us;

static bool raw_pressed(void)
{
#if CONFIG_LAMP_INPUT_TOUCH
    bool pressed = touch_is_touched();
#elif CONFIG_LAMP_BUTTON_ACTIVE_LOW
    bool pressed = gpio_get_level(CONFIG_LAMP_BUTTON_GPIO) == 0;
#else
    bool pressed = gpio_get_level(CONFIG_LAMP_BUTTON_GPIO) != 0;
#endif
    return pressed || esp_timer_get_time() < s_inject_until_us;
}

static void button_task(void *arg)
{
    bool stable = false;    /* debounced state */
    uint8_t counter = 0;
    uint32_t held_ms = 0;
    bool holding = false;
    /* The factory-reset gesture is only armed for a press that starts shortly after boot. Such a press
     * otherwise acts normally (tap toggles, hold dims) until the reset hold time is reached. */
    bool boot_press = false;
    bool reset_fired = false;

    TickType_t wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(POLL_MS));

        bool raw = raw_pressed();
        if (raw != stable) {
            if (++counter >= DEBOUNCE_COUNT) {
                stable = raw;
                counter = 0;
                if (stable) {
                    /* press */
                    held_ms = 0;
                    holding = false;
                    reset_fired = false;
                    boot_press = (esp_timer_get_time() / 1000) < CONFIG_LAMP_BOOT_RESET_WINDOW_MS;
                    if (boot_press) {
                        ESP_LOGI(TAG, "Press during boot window: hold for %d ms to factory reset",
                                 CONFIG_LAMP_BOOT_RESET_HOLD_MS);
                    }
                } else {
                    /* release */
                    if (reset_fired) {
                        /* The reset already took over; nothing else to do. */
                    } else if (holding) {
                        s_cb(BUTTON_EVENT_HOLD_END, 0);
                    } else {
                        s_cb(BUTTON_EVENT_SHORT_PRESS, 0);
                    }
                    holding = false;
                    boot_press = false;
                }
            }
        } else {
            counter = 0;
        }

        if (!stable || reset_fired) {
            continue;
        }
        held_ms += POLL_MS;
        if (boot_press && held_ms >= CONFIG_LAMP_BOOT_RESET_HOLD_MS) {
            reset_fired = true;
            s_cb(BUTTON_EVENT_BOOT_RESET, 0);
        } else if (!holding) {
            if (held_ms >= CONFIG_LAMP_HOLD_THRESHOLD_MS) {
                holding = true;
                s_cb(BUTTON_EVENT_HOLD_START, 0);
            }
        } else {
            s_cb(BUTTON_EVENT_HOLD_TICK, POLL_MS);
        }
    }
}

esp_err_t button_init(button_callback_t cb)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "callback required");
    s_cb = cb;

#if CONFIG_LAMP_INPUT_TOUCH
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch init failed");
#else
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_LAMP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
#if CONFIG_LAMP_BUTTON_PULLUP
        .pull_up_en = GPIO_PULLUP_ENABLE,
#endif
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio config failed");
#endif

    BaseType_t ok = xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    return ESP_OK;
}

void button_inject_press(uint32_t duration_ms)
{
    s_inject_until_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
}
