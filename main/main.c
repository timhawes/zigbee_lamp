#include "esp_log.h"
#include "nvs_flash.h"

#include "interference_led.h"
#include "light_output.h"
#include "local_control.h"
#include "test_console.h"
#include "zigbee_light.h"

static const char *TAG = "lamp";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "Zigbee dimmable lamp starting");
    /* Start the button first so the boot-time factory-reset gesture is caught promptly. */
    ESP_ERROR_CHECK(local_control_init());
    ESP_ERROR_CHECK(light_output_init());
    ESP_ERROR_CHECK(interference_led_start());
    ESP_ERROR_CHECK(zigbee_light_start());
    ESP_ERROR_CHECK(test_console_start());
}
