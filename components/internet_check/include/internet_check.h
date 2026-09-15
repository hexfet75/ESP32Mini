#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    INTERNET_STATUS_UNKNOWN,
    INTERNET_STATUS_AVAILABLE,
    INTERNET_STATUS_UNAVAILABLE   // WiFi connesso ma senza uscita Internet
} internet_status_t;

typedef void (*internet_check_status_cb_t)(internet_status_t status);

// Esegue un singolo check sincrono (bloccante, utile per test rapidi)
bool internet_check_now(void);

// Avvia un task che verifica periodicamente e notifica i cambi di stato
esp_err_t internet_check_start(internet_check_status_cb_t status_cb, uint32_t interval_ms);