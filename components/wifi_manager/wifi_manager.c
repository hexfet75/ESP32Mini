#include "wifi_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi_manager";
#define NVS_NAMESPACE "wifi_cfg"
#define WIFI_TIMEOUT_MS (30 * 1000)

static EventGroupHandle_t wifi_event_group;
#define CONNECTED_BIT BIT0
#define FAIL_BIT      BIT1

static wifi_manager_status_cb_t user_status_cb = NULL;

static void notify_status(wifi_manager_status_t status)
{
    if (user_status_cb) user_status_cb(status);
}

// ---------- Gestione credenziali su NVS ----------

esp_err_t wifi_manager_set_credentials(uint8_t slot, const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    char key_ssid[16], key_pass[16];
    snprintf(key_ssid, sizeof(key_ssid), "ssid%d", slot);
    snprintf(key_pass, sizeof(key_pass), "pass%d", slot);

    err = nvs_set_str(handle, key_ssid, ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, key_pass, password);
    if (err == ESP_OK) err = nvs_commit(handle);

    nvs_close(handle);
    return err;
}

esp_err_t wifi_manager_get_credentials(uint8_t slot, char *ssid_out, size_t ssid_len,
                                        char *pass_out, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    char key_ssid[16], key_pass[16];
    snprintf(key_ssid, sizeof(key_ssid), "ssid%d", slot);
    snprintf(key_pass, sizeof(key_pass), "pass%d", slot);

    err = nvs_get_str(handle, key_ssid, ssid_out, &ssid_len);
    if (err == ESP_OK) err = nvs_get_str(handle, key_pass, pass_out, &pass_len);

    nvs_close(handle);
    return err;
}

// ---------- Event handler WiFi ----------

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(wifi_event_group, FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    }
}

// ---------- Tentativo di connessione a uno slot ----------

static bool try_connect(uint8_t slot)
{
    char ssid[33] = {0};
    char pass[65] = {0};

    if (wifi_manager_get_credentials(slot, ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
        ESP_LOGW(TAG, "Nessuna credenziale salvata per slot %d", slot);
        return false;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

    xEventGroupClearBits(wifi_event_group, CONNECTED_BIT | FAIL_BIT);

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    ESP_LOGI(TAG, "Tentativo connessione a '%s' (slot %d)...", ssid, slot);

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group, CONNECTED_BIT | FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    return (bits & CONNECTED_BIT) != 0;
}

// ---------- Task principale: gestisce fallback e riconnessione ----------

static void wifi_manager_task(void *arg)
{
    while (1) {
        notify_status(WIFI_STATUS_CONNECTING_PRIMARY);
        if (try_connect(0)) {
            notify_status(WIFI_STATUS_CONNECTED);
        } else {
            notify_status(WIFI_STATUS_CONNECTING_SECONDARY);
            if (try_connect(1)) {
                notify_status(WIFI_STATUS_CONNECTED);
            } else {
                notify_status(WIFI_STATUS_FAILED_BOTH);
                vTaskDelay(pdMS_TO_TICKS(5000)); // pausa prima di riprovare il ciclo
                continue;
            }
        }

        // Connesso: resta in ascolto finché non arriva una disconnessione
        xEventGroupWaitBits(wifi_event_group, FAIL_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        ESP_LOGW(TAG, "Connessione persa, riavvio ciclo di connessione...");
        notify_status(WIFI_STATUS_DISCONNECTED);
    }
}

// ---------- Init pubblico ----------

esp_err_t wifi_manager_init(wifi_manager_status_cb_t status_cb)
{
    user_status_cb = status_cb;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    xTaskCreate(wifi_manager_task, "wifi_manager", 4096, NULL, 5, NULL);
    return ESP_OK;
}