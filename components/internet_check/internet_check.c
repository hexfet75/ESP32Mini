#include "internet_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "internet_check";
#define CHECK_URL "http://connectivitycheck.gstatic.com/generate_204"
#define HTTP_TIMEOUT_MS 5000

static internet_check_status_cb_t user_cb = NULL;
static internet_status_t last_status = INTERNET_STATUS_UNKNOWN;

bool internet_check_now(void)
{
    esp_http_client_config_t config = {
        .url = CHECK_URL,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .method = HTTP_METHOD_HEAD,   // solo header, non serve il body -> più leggero
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    bool ok = false;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        // 204 è la risposta attesa; accettiamo anche 200 per tolleranza
        ok = (status == 204 || status == 200);
        ESP_LOGI(TAG, "Check completato, HTTP status: %d -> %s", status, ok ? "OK" : "FAIL");
    } else {
        ESP_LOGW(TAG, "Check fallito: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return ok;
}

static void internet_check_task(void *arg)
{
    uint32_t interval_ms = (uint32_t)(uintptr_t)arg;

    while (1) {
        bool ok = internet_check_now();
        internet_status_t new_status = ok ? INTERNET_STATUS_AVAILABLE : INTERNET_STATUS_UNAVAILABLE;

        if (new_status != last_status) {
            last_status = new_status;
            if (user_cb) user_cb(new_status);
        }

        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

esp_err_t internet_check_start(internet_check_status_cb_t status_cb, uint32_t interval_ms)
{
    user_cb = status_cb;
    xTaskCreate(internet_check_task, "internet_check", 4096,
                (void *)(uintptr_t)interval_ms, 5, NULL);
    return ESP_OK;
}