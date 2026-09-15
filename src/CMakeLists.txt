idf_component_register(
    SRCS "main.c"
    REQUIRES status_led wifi_manager internet_check mqtt_manager driver
)
