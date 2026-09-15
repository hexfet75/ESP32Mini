#include "status_led.h"
#include "wifi_manager.h"
#include "internet_check.h"
#include "mqtt_manager.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "main";
#define MQTT_SLEEP_TOPIC "esp32/cmd/sleep"
#define MQTT_SLEEP_PAYLOAD "sleep"
#define SLEEP_TIMER_US (5ULL * 60ULL * 1000000ULL)

static void on_wifi_status(wifi_manager_status_t status)
{
    switch (status) {
        case WIFI_STATUS_CONNECTING_PRIMARY:
        case WIFI_STATUS_CONNECTING_SECONDARY:
            status_led_set(STATUS_CONNECTING);
            break;
        case WIFI_STATUS_CONNECTED:
            status_led_set(STATUS_OK);  // verrà eventualmente corretto dal check Internet
            break;
        case WIFI_STATUS_FAILED_BOTH:
            status_led_set(STATUS_ERROR);
            break;
        case WIFI_STATUS_DISCONNECTED:
            status_led_set(STATUS_WARNING);
            break;
    }
}
static bool internet_ok = false;
static volatile bool mqtt_ok = false;
static bool mqtt_started = false;

static void publish_mqtt_status(void)
{
    if (mqtt_ok) {
        mqtt_manager_publish("esp32/test/status", "online", 1, true);
    }
}

static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "Deep sleep: risveglio tra 5 minuti");

    mqtt_ok = false;
    mqtt_manager_stop();
    esp_sleep_enable_timer_wakeup(SLEEP_TIMER_US);
    esp_deep_sleep_start();
}

static void mqtt_status_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        publish_mqtt_status();
    }
}

static void update_led_state(void)
{
    if (!internet_ok) {
        status_led_set(STATUS_WARNING);      // problema di rete: priorità massima
    } else if (!mqtt_ok) {
        status_led_set(STATUS_MQTT_ERROR);   // rete ok, ma MQTT giù
    } else {
        status_led_set(STATUS_OK);           // tutto ok
    }
}
static void on_mqtt_status(mqtt_mgr_status_t status)
{
    mqtt_ok = (status == MQTT_MGR_CONNECTED);
    if (mqtt_ok) {
        publish_mqtt_status();
        mqtt_manager_subscribe(MQTT_SLEEP_TOPIC, 1);
    }
    update_led_state();
}
static void on_mqtt_message(const char *topic, const char *payload, int len)
{
    ESP_LOGI(TAG, "Messaggio ricevuto su '%s': %.*s", topic, len, payload);

    if (strcmp(topic, MQTT_SLEEP_TOPIC) == 0 &&
        len == (int)(sizeof(MQTT_SLEEP_PAYLOAD) - 1) &&
        strcmp(payload, MQTT_SLEEP_PAYLOAD) == 0) {
        enter_deep_sleep();
    }
}

// Nella callback di internet_check, avvia MQTT solo quando c'è davvero Internet:
static void on_internet_status(internet_status_t status)
{
    internet_ok = (status == INTERNET_STATUS_AVAILABLE);
    if (internet_ok && !mqtt_started) {
        mqtt_manager_start();
        mqtt_started = true;
    }
    update_led_state();
}

void app_main(void)
{
    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    if (wakeup_cause == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Risveglio dal deep sleep tramite timer");
    } else {
        ESP_LOGI(TAG, "Avvio normale, wakeup cause: %d", wakeup_cause);
    }

    status_led_init();
    status_led_set(STATUS_BOOT);

    wifi_manager_init(on_wifi_status);
    wifi_manager_set_credentials(0, "OSPITI2.4G", "ospiti-CONMAR");
    wifi_manager_set_credentials(1, "SSID_SECONDARIA", "password_secondaria");
    wifi_manager_start();

    // Configurazione MQTT (solo al primo avvio, poi resta in NVS)
    //mqtt_manager_set_config("mqtts://ufficio.conmargroup.it:8883", "youruser", "passwd");
    mqtt_manager_set_config(
    "mqtt://192.168.0.6:1883",
    "youruser",
    "passwd"
    );
    mqtt_manager_init(on_mqtt_status, on_mqtt_message);
    xTaskCreate(mqtt_status_task, "mqtt_status", 3072, NULL, 4, NULL);
    // Il check Internet parte comunque; se il WiFi non è connesso,
    // internet_check_now() fallirà semplicemente (timeout), senza crash
    internet_check_start(on_internet_status, 30000); // ogni 30 secondi
}