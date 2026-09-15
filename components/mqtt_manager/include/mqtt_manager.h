#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    MQTT_MGR_DISCONNECTED,
    MQTT_MGR_CONNECTING,
    MQTT_MGR_CONNECTED,
    MQTT_MGR_ERROR
} mqtt_mgr_status_t;

typedef void (*mqtt_mgr_status_cb_t)(mqtt_mgr_status_t status);
typedef void (*mqtt_mgr_message_cb_t)(const char *topic, const char *payload, int payload_len);

// Configurazione salvata in NVS (uri, username, password)
esp_err_t mqtt_manager_set_config(const char *broker_uri, const char *username, const char *password);

esp_err_t mqtt_manager_init(mqtt_mgr_status_cb_t status_cb, mqtt_mgr_message_cb_t message_cb);
esp_err_t mqtt_manager_start(void);   // avvia connessione (chiamare solo quando Internet è disponibile)
esp_err_t mqtt_manager_stop(void);

esp_err_t mqtt_manager_publish(const char *topic, const char *payload, int qos, bool retain);
esp_err_t mqtt_manager_subscribe(const char *topic, int qos);