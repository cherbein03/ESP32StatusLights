idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp-zigbee-lib esp_timer freertos led_strip nvs_flash
)