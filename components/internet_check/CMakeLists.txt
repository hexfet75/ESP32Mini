idf_component_register(
    SRCS "internet_check.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos esp_http_client
)