#pragma once

#include "esp_err.h"

/* Shows touch interference on the onboard WS2812 (CONFIG_LAMP_INTERFERENCE_LED): green when clean, through
 * yellow, to red at the touch ON threshold. Blue while a scan waits for its first result. No-op when disabled. */
esp_err_t interference_led_start(void);
