idf_component_register(
    SRCS "mqtt_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos mqtt nvs_flash esp-tls mbedtls
)