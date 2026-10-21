#include "interference_led.h"
#include "sdkconfig.h"

#if CONFIG_LAMP_INTERFERENCE_LED

#include <math.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "led_strip.h"
#include "touch.h"

#define UPDATE_MS  100 /* also limits how often the LED's data line toggles near the touch wiring */
#define BRIGHTNESS 40  /* of 255: the onboard LED is very bright */

static const char *TAG = "interference_led";
static led_strip_handle_t s_strip;

static void led_task(void *arg)
{
    uint32_t last = UINT32_MAX;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(UPDATE_MS));
        bool scanning;
        int pm = touch_interference_permille(&scanning);
        uint32_t r, g, b = 0;
        if (pm < 0) {
            r = g = 0; /* scan running, no result yet */
            b = BRIGHTNESS;
        } else {
            float x = fminf((float)pm / CONFIG_LAMP_TOUCH_ON_PERMILLE, 1.0f);
            r = lroundf(fminf(2.0f * x, 1.0f) * BRIGHTNESS);
            g = lroundf(fminf(2.0f * (1.0f - x), 1.0f) * BRIGHTNESS);
        }
        uint32_t rgb = (r << 16) | (g << 8) | b;
        if (rgb != last) {
            led_strip_set_pixel(s_strip, 0, r, g, b);
            led_strip_refresh(s_strip);
            last = rgb;
        }
    }
}

esp_err_t interference_led_start(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_LAMP_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip), TAG, "LED init failed");
    led_strip_clear(s_strip);
    BaseType_t ok = xTaskCreate(led_task, "intf_led", 3072, NULL, 2, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    return ESP_OK;
}

#elif CONFIG_LAMP_OUTPUT_PWM

#include "esp_check.h"
#include "led_strip.h"

static const char *TAG = "interference_led";
static led_strip_handle_t s_strip;

/* Indicator disabled: send the onboard LED one "off" frame so it can't show a colour latched from noise on its
 * data line during power-up. The strip stays allocated so the RMT holds the line idle (low). */
esp_err_t interference_led_start(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_LAMP_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip), TAG, "LED init failed");
    return led_strip_clear(s_strip);
}

#else

esp_err_t interference_led_start(void)
{
    return ESP_OK;
}

#endif
