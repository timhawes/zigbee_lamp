#pragma once

#include "esp_err.h"

/* Start the USB Serial/JTAG test console (no-op unless CONFIG_LAMP_TEST_CONSOLE). */
esp_err_t test_console_start(void);
