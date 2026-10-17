#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define LAMP_ENDPOINT   11
#define LAMP_LEVEL_MIN  1
#define LAMP_LEVEL_MAX  254

/* Restore persisted state, drive the output and start the Zigbee stack task. */
esp_err_t zigbee_light_start(void);

/* Current light state as held by the ZCL attribute table. */
void zigbee_light_get_state(bool *on, uint8_t *level);

/*
 * Apply a locally generated state change (button). The output follows immediately.
 * When update_attributes is true, the ZCL attributes are updated too (which drives
 * attribute reporting); during a dimming ramp this is throttled by the caller.
 */
void zigbee_light_set_local(bool on, uint8_t level, bool update_attributes);

/* Leave the network (if joined), wipe all state and restart into pairing mode. */
void zigbee_light_factory_reset(void);

/* Active scan on all channels; prints one SCAN line per beacon, then "SCAN done" (test console). */
esp_err_t zigbee_light_scan(void);

/* Print a single machine-readable STATUS line (test console). */
void zigbee_light_print_status(void);
