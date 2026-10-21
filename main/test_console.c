#include "test_console.h"

#include "sdkconfig.h"

#if CONFIG_LAMP_TEST_CONSOLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_console.h"
#include "driver/temperature_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "button.h"
#include "light_output.h"
#if CONFIG_LAMP_INPUT_TOUCH
#include "touch.h"
#endif
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

#if CONFIG_LAMP_INPUT_TOUCH
static void print_touch(void)
{
    touch_stats_t st;
    touch_get_stats(&st);
    bool scanning;
    int interference = touch_interference_permille(&scanning);
    printf("TOUCH raw=%.0f baseline=%.0f delta=%d edges=%lu calibrated=%d touched=%d interference=%d%s\n", st.raw_ns,
           st.baseline_ns, st.delta_permille, (unsigned long)st.edges, st.calibrated, st.touched, interference,
           scanning ? " (scan)" : "");
}

static int cmd_touch(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "watch") == 0) {
        /* 10 Hz for the given number of seconds; used to tune the thresholds. */
        uint32_t n = strtoul(argv[2], NULL, 0) * 10;
        for (uint32_t i = 0; i < n; i++) {
            print_touch();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "raw") == 0) {
        /* Every 10 ms frame, printed by the touch task in the background, so other commands still work. */
        touch_stream(strtoul(argv[2], NULL, 0) * 1000);
        return 0;
    }
#if CONFIG_LAMP_OUTPUT_PWM
    if (argc >= 2 && strcmp(argv[1], "scan") == 0) {
        /* Sweeps the PWM hands-off and reports the offset it causes; "touch scan 0" stops. */
        uint32_t s = argc >= 3 ? strtoul(argv[2], NULL, 0) : 60;
        touch_scan(s * 1000);
        printf("OK touch scan %lu\n", (unsigned long)s);
        return 0;
    }
#endif
    if (argc != 1) {
        printf("usage: touch [watch <s> | raw <s> | scan [s]]\n");
        return 1;
    }
    print_touch();
    return 0;
}
#endif

#if CONFIG_LAMP_OUTPUT_PWM
static int cmd_pwm(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "phase") == 0) {
        esp_err_t err = light_output_set_pwm_phase(strtoul(argv[2], NULL, 0));
        printf(err == ESP_OK ? "OK pwm phase\n" : "ERR pwm phase %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "test") == 0) {
        /* Hold the PWM at a duty (per mille of full scale) for measurements; touches are ignored meanwhile. */
        bool off = strcmp(argv[2], "off") == 0;
        light_output_set_test_duty(off ? -1.0f : strtoul(argv[2], NULL, 0) / 1000.0f);
        printf("OK pwm test %s\n", argv[2]);
        return 0;
    }
    printf("usage: pwm phase <us> | pwm test <per mille> | pwm test off\n");
    return 1;
}
#endif

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
#if CONFIG_LAMP_INPUT_TOUCH
        { .command = "touch", .help = "Touch sensor readings (ns, delta in per mille): touch [watch <s> | raw <s> | scan [s]]",
          .func = cmd_touch },
#endif
#if CONFIG_LAMP_OUTPUT_PWM
        { .command = "pwm", .help = "PWM tests: pwm phase <us> (re-lock after the touch drive edge) | pwm test <per mille>|off (hold a duty)",
          .func = cmd_pwm },
#endif
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
