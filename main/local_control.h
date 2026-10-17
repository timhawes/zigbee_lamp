#pragma once

#include "esp_err.h"

/* Wire the button to the light: short press toggles, hold dims, boot gesture resets. */
esp_err_t local_control_init(void);
