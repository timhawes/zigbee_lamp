#include "light_output.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "sdkconfig.h"

#define RENDER_PERIOD_MS 10
/* First-order smoothing in the perceptual (level) domain; ~120 ms time constant. */
#define SMOOTHING_ALPHA  (RENDER_PERIOD_MS / 120.0f)
#define SNAP_DISTANCE    (0.5f / 254.0f) /* within half a level of the target: done */
#define GAMMA            2.2f

static const char *TAG = "light_output";

static led_strip_handle_t s_strip;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_target;          /* perceptual level 0..1 (0 = off) */
static light_effect_t s_effect;
static int64_t s_identify_until_us;
static volatile uint32_t s_rgb; /* last bytes written to the LED */

static float level_to_perceptual(bool on, uint8_t level)
{
    if (!on || level == 0) {
        return 0.0f;
    }
    if (level > 254) {
        level = 254;
    }
    return level / 254.0f;
}

/* Channel values are linear brightness 0..1. min_one keeps a lit channel from rounding to zero.
 * Returns the bytes packed as 0xRRGGBB. */
static uint32_t to_rgb(float r, float g, float b, bool min_one)
{
    const float max = CONFIG_LAMP_LED_MAX_BRIGHTNESS;
    uint32_t ri = lroundf(r * max), gi = lroundf(g * max), bi = lroundf(b * max);
    if (min_one) {
        if (r > 0.0f && ri == 0) ri = 1;
        if (g > 0.0f && gi == 0) gi = 1;
        if (b > 0.0f && bi == 0) bi = 1;
    }
    return (ri << 16) | (gi << 8) | bi;
}

static void render_task(void *arg)
{
    float current = 0.0f;
    uint32_t last_rgb = UINT32_MAX;
    TickType_t wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(RENDER_PERIOD_MS));

        portENTER_CRITICAL(&s_lock);
        float target = s_target;
        light_effect_t effect = s_effect;
        int64_t identify_until = s_identify_until_us;
        portEXIT_CRITICAL(&s_lock);

        int64_t now_us = esp_timer_get_time();
        if (effect == LIGHT_EFFECT_NONE && now_us < identify_until) {
            effect = LIGHT_EFFECT_IDENTIFY;
        }

        current += (target - current) * SMOOTHING_ALPHA;
        if (fabsf(target - current) < SNAP_DISTANCE) {
            current = target;
        }

        /* The minimum-brightness clamp only applies while on, so fading to off ends cleanly. */
        bool min_one = target > 0.0f;
        float lin = powf(current, GAMMA);
        float r = lin, g = lin, b = lin;
        uint32_t ms = (uint32_t)(now_us / 1000);
        switch (effect) {
        case LIGHT_EFFECT_PAIRING: {
            /* 2 s breathing period, blue */
            float phase = (ms % 2000) / 2000.0f;
            float v = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * phase);
            r = g = 0.0f;
            b = 0.02f + 0.3f * v;
            min_one = true;
            break;
        }
        case LIGHT_EFFECT_IDENTIFY: {
            float v = ((ms / 500) % 2) ? 0.0f : 1.0f;
            r = g = b = v;
            break;
        }
        case LIGHT_EFFECT_RESET: {
            float v = ((ms / 100) % 2) ? 0.0f : 0.5f;
            r = v;
            g = b = 0.0f;
            break;
        }
        default:
            break;
        }

        /* Only push to the LED when the bytes sent to it change. */
        uint32_t rgb = to_rgb(r, g, b, min_one);
        if (rgb != last_rgb) {
            led_strip_set_pixel(s_strip, 0, rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);
            led_strip_refresh(s_strip);
            last_rgb = rgb;
            s_rgb = rgb;
        }
    }
}

esp_err_t light_output_init(void)
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

    BaseType_t ok = xTaskCreate(render_task, "light_out", 3072, NULL, 6, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "render task create failed");
    return ESP_OK;
}

void light_output_set(bool on, uint8_t level)
{
    float b = level_to_perceptual(on, level);
    portENTER_CRITICAL(&s_lock);
    s_target = b;
    portEXIT_CRITICAL(&s_lock);
}

void light_output_set_effect(light_effect_t effect)
{
    portENTER_CRITICAL(&s_lock);
    s_effect = effect;
    portEXIT_CRITICAL(&s_lock);
}

void light_output_identify_for(uint32_t duration_ms)
{
    portENTER_CRITICAL(&s_lock);
    s_identify_until_us = duration_ms ? esp_timer_get_time() + (int64_t)duration_ms * 1000 : 0;
    portEXIT_CRITICAL(&s_lock);
}

uint32_t light_output_get_rgb(void)
{
    return s_rgb;
}
