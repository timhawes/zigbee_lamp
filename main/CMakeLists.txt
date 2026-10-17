idf_component_register(
    SRCS "main.c" "zigbee_light.c" "light_output.c" "button.c" "local_control.c" "test_console.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_tsens esp_timer nvs_flash console
)
