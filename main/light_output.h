#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Status effects that temporarily override the normal light output. */
typedef enum {
    LIGHT_EFFECT_NONE = 0,
    LIGHT_EFFECT_PAIRING,  /* slow blue breathing while searching for a network */
    LIGHT_EFFECT_IDENTIFY, /* white blinking while IdentifyTime > 0 */
    LIGHT_EFFECT_RESET,    /* fast red blinking before a factory reset */
} light_effect_t;

esp_err_t light_output_init(void);

/* Set the target light state. Level is the ZCL CurrentLevel (1-254). */
void light_output_set(bool on, uint8_t level);

void light_output_set_effect(light_effect_t effect);

/* Show the identify effect for a limited time (Identify TriggerEffect). */
void light_output_identify_for(uint32_t duration_ms);

/* The bytes last written to the LED, packed as 0xRRGGBB (for diagnostics). */
uint32_t light_output_get_rgb(void);
