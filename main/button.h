#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    BUTTON_EVENT_SHORT_PRESS, /* released before the hold threshold */
    BUTTON_EVENT_HOLD_START,  /* held past the hold threshold */
    BUTTON_EVENT_HOLD_TICK,   /* every poll period while held (after HOLD_START) */
    BUTTON_EVENT_HOLD_END,    /* released after HOLD_START */
    BUTTON_EVENT_BOOT_RESET,  /* factory-reset gesture: press started right after boot, held for the reset time */
} button_event_t;

/* Called from the button task. dt_ms is the poll period (for HOLD_TICK). */
typedef void (*button_callback_t)(button_event_t event, uint32_t dt_ms);

esp_err_t button_init(button_callback_t cb);

/* Simulate the button being held down for duration_ms (test console). */
void button_inject_press(uint32_t duration_ms);
