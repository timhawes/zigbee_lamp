#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Charge-time touch sensor: SEND drives a square wave through a high-value resistor into the pad,
 * RECV (on the pad side) times how long each edge takes to cross the input threshold. A finger adds
 * capacitance, so the edges get slower. */

typedef struct {
    float raw_ns;       /* mean edge delay over the last 20 ms */
    float baseline_ns;  /* untouched reference */
    int delta_permille; /* (raw - baseline) / baseline */
    uint32_t edges;     /* edges measured in the last 20 ms (expect 2 x 20 ms x drive frequency) */
    bool calibrated;
    bool touched;
} touch_stats_t;

esp_err_t touch_init(void);

bool touch_is_touched(void);

void touch_get_stats(touch_stats_t *out);

/* Microseconds into the current touch drive period (0 to 1e6 / LAMP_TOUCH_DRIVE_HZ - 1), read directly from the
 * drive timer; -1 if touch sensing isn't running. light_output.c uses it to keep the PWM edges away from the
 * measurement. Cheap and safe inside a critical section. */
int32_t touch_drive_time_us(void);

/* Print every 10 ms frame for duration_ms: "TR <ms> <frame delta> <window delta> <edges> <touched> <output>". */
void touch_stream(uint32_t duration_ms);

/* Interference meter, in per mille of the baseline (the units of the touch thresholds).
 * Normally: the recent peak |delta| while untouched (a finger approaching the pad shows too).
 * During a scan: the worst |delta| found in the last complete sweep, or -1 before the first one.
 * *scanning (may be NULL) says which. */
int touch_interference_permille(bool *scanning);

/* Hands-off interference scan for duration_ms (0 stops it). Each 2.25 s cycle turns the PWM off for 250 ms to
 * take a reference, then ramps the duty linearly from 0 to 100 % over 2 s, measuring how far the reading moves
 * from the reference. Touch input is ignored meanwhile. Prints a "SCAN" line per cycle. PWM output only. */
void touch_scan(uint32_t duration_ms);
