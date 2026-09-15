#pragma once
#include "esp_err.h"

typedef enum {
    WIFI_STATUS_DISCONNECTED,
    WIFI_STATUS_CONNECTING_PRIMARY,
    WIFI_STATUS_CONNECTING_SECONDARY,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_FAILED_BOTH   // nessuna delle due reti raggiungibile
} wifi_manager_status_t;

// Callback opzionale: wifi_manager notifica i cambi di stato,
// ma non sa nulla di status_led -> resta un componente indipendente
typedef void (*wifi_manager_status_cb_t)(wifi_manager_status_t status);

// Slot: 0 = primaria, 1 = secondaria
esp_err_t wifi_manager_set_credentials(uint8_t slot, const char *ssid, const char *password);
esp_err_t wifi_manager_get_credentials(uint8_t slot, char *ssid_out, size_t ssid_len,
                                        char *pass_out, size_t pass_len);

esp_err_t wifi_manager_init(wifi_manager_status_cb_t status_cb);
esp_err_t wifi_manager_start(void);   // avvia il task che gestisce connessione/fallback