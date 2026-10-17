#include "local_control.h"

#include <stdbool.h>
#include "esp_log.h"
#include "sdkconfig.h"

#include "button.h"
#include "zigbee_light.h"

#define ATTR_UPDATE_INTERVAL_MS 100 /* throttle attribute writes while ramping */

static const char *TAG = "local_control";

static bool s_last_dir_up;   /* direction of the previous ramp */
static bool s_ramp_up;
static float s_ramp_level;
static uint32_t s_since_attr_update_ms;

static void start_ramp(void)
{
    bool on;
    uint8_t level;
    zigbee_light_get_state(&on, &level);

    if (!on) {
        /* Dimming up from off starts at the minimum level. */
        level = LAMP_LEVEL_MIN;
        s_ramp_up = true;
    } else if (level >= LAMP_LEVEL_MAX) {
        s_ramp_up = false;
    } else if (level <= LAMP_LEVEL_MIN) {
        s_ramp_up = true;
    } else {
        s_ramp_up = !s_last_dir_up;
    }
    s_last_dir_up = s_ramp_up;
    s_ramp_level = level;
    s_since_attr_update_ms = 0;

    ESP_LOGI(TAG, "Dim %s from %d", s_ramp_up ? "up" : "down", level);
    zigbee_light_set_local(true, level, true);
}

static void step_ramp(uint32_t dt_ms)
{
    const float step = (float)(LAMP_LEVEL_MAX - LAMP_LEVEL_MIN) * dt_ms / CONFIG_LAMP_DIM_SWEEP_MS;
    float next = s_ramp_level + (s_ramp_up ? step : -step);
    if (next > LAMP_LEVEL_MAX) next = LAMP_LEVEL_MAX;
    if (next < LAMP_LEVEL_MIN) next = LAMP_LEVEL_MIN;

    uint8_t prev_level = (uint8_t)(s_ramp_level + 0.5f);
    uint8_t level = (uint8_t)(next + 0.5f);
    s_ramp_level = next;

    s_since_attr_update_ms += dt_ms;
    bool update_attrs = s_since_attr_update_ms >= ATTR_UPDATE_INTERVAL_MS;
    if (level != prev_level || update_attrs) {
        zigbee_light_set_local(true, level, update_attrs);
        if (update_attrs) {
            s_since_attr_update_ms = 0;
        }
    }
}

static void end_ramp(void)
{
    uint8_t level = (uint8_t)(s_ramp_level + 0.5f);
    ESP_LOGI(TAG, "Dim stopped at %d", level);
    zigbee_light_set_local(true, level, true);
}

static void on_button(button_event_t event, uint32_t dt_ms)
{
    switch (event) {
    case BUTTON_EVENT_SHORT_PRESS: {
        bool on;
        uint8_t level;
        zigbee_light_get_state(&on, &level);
        ESP_LOGI(TAG, "Toggle -> %s", on ? "off" : "on");
        zigbee_light_set_local(!on, level, true);
    } break;
    case BUTTON_EVENT_HOLD_START:
        start_ramp();
        break;
    case BUTTON_EVENT_HOLD_TICK:
        step_ramp(dt_ms);
        break;
    case BUTTON_EVENT_HOLD_END:
        end_ramp();
        break;
    case BUTTON_EVENT_BOOT_RESET:
        ESP_LOGW(TAG, "Factory reset requested by button");
        zigbee_light_factory_reset();
        break;
    }
}

esp_err_t local_control_init(void)
{
    return button_init(on_button);
}
