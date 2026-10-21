#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Status effects that temporarily override the normal light output. */
typedef enum {
    LIGHT_EFFECT_NONE = 0,
    LIGHT_EFFECT_PAIRING,  /* slow blue breathing while searching for a network (CONFIG_LAMP_PAIRING_INDICATION) */
    LIGHT_EFFECT_IDENTIFY, /* 1 Hz white blinking while IdentifyTime > 0 */
    LIGHT_EFFECT_RESET,    /* 2 Hz red blinking before a factory reset */
} light_effect_t;

esp_err_t light_output_init(void);

/* Set the target light state. Level is the ZCL CurrentLevel (1-254). */
void light_output_set(bool on, uint8_t level);

void light_output_set_effect(light_effect_t effect);

/* Show the identify effect for a limited time (Identify TriggerEffect). */
void light_output_identify_for(uint32_t duration_ms);

/* The value last written to the output, for diagnostics: 0xRRGGBB for the WS2812, the duty count for PWM. */
uint32_t light_output_get_raw(void);

/* Force the PWM duty to a fraction of full scale (0..1), bypassing the light state, for the touch interference
 * scan. A negative value returns to normal output. No effect on the WS2812 output. */
void light_output_set_test_duty(float duty);

/* The current override, or a negative value when there is none. */
float light_output_get_test_duty(void);

/* Re-lock the PWM so its counter starts this many microseconds after a touch drive edge (0 to one PWM period).
 * For experiments; the edge placement assumes the default phase. ESP_ERR_NOT_SUPPORTED unless phase-locked. */
esp_err_t light_output_set_pwm_phase(uint32_t us);
