#pragma once

typedef enum {
    STATUS_BOOT,
    STATUS_CONNECTING,
    STATUS_OK,
    STATUS_WARNING,
    STATUS_ERROR,
    STATUS_MQTT_ERROR,
    STATUS_OFF
} system_status_t;

void status_led_init(void);
void status_led_set(system_status_t status);