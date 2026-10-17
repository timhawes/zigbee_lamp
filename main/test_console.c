#include "test_console.h"

#include "sdkconfig.h"

#if CONFIG_LAMP_TEST_CONSOLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_console.h"
#include "driver/temperature_sensor.h"

#include "button.h"
#include "zigbee_light.h"

static const char *TAG = "test_console";

static int cmd_btn(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "press") == 0) {
        uint32_t ms = argc >= 3 ? strtoul(argv[2], NULL, 0) : 150;
        button_inject_press(ms);
        printf("OK btn press %lu\n", (unsigned long)ms);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "hold") == 0) {
        uint32_t ms = strtoul(argv[2], NULL, 0);
        button_inject_press(ms);
        printf("OK btn hold %lu\n", (unsigned long)ms);
        return 0;
    }
    printf("usage: btn press [ms] | btn hold <ms>\n");
    return 1;
}

static int cmd_status(int argc, char **argv)
{
    zigbee_light_print_status();
    return 0;
}

static int cmd_temp(int argc, char **argv)
{
    static temperature_sensor_handle_t s_tsens;
    if (!s_tsens) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK || temperature_sensor_enable(s_tsens) != ESP_OK) {
            printf("ERR temp\n");
            return 1;
        }
    }
    float c = 0;
    temperature_sensor_get_celsius(s_tsens, &c);
    printf("TEMP %.1f\n", c);
    return 0;
}

static int cmd_scan(int argc, char **argv)
{
    esp_err_t err = zigbee_light_scan();
    if (err != ESP_OK) {
        printf("ERR scan %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    printf("OK factory_reset\n");
    zigbee_light_factory_reset();
    return 0;
}

esp_err_t test_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "lamp>";
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl), TAG, "repl");

    const esp_console_cmd_t cmds[] = {
        { .command = "btn", .help = "Simulate the button: btn press [ms] | btn hold <ms>", .func = cmd_btn },
        { .command = "status", .help = "Print light and network status", .func = cmd_status },
        { .command = "temp", .help = "Print the chip temperature", .func = cmd_temp },
        { .command = "scan", .help = "List nearby Zigbee networks (beacons)", .func = cmd_scan },
        { .command = "factory_reset", .help = "Leave network and factory reset", .func = cmd_factory_reset },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "register");
    }
    esp_console_register_help_command();
    return esp_console_start_repl(repl);
}

#else

esp_err_t test_console_start(void)
{
    return ESP_OK;
}

#endif
