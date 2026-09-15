#include "status_led.h"
#include "wifi_manager.h"
#include "internet_check.h"
#include "mqtt_manager.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "main";
static volatile bool mqtt_ok = false;
#define MQTT_SLEEP_TOPIC "esp32/cmd/sleep"
#define MQTT_SLEEP_PAYLOAD "sleep"
#define MQTT_RESTART_TOPIC "esp32/cmd/restart"
#define MQTT_RESTART_PAYLOAD "restart"
#define ADS1115_ADDR 0x48
#define ADS1115_SDA GPIO_NUM_8
#define ADS1115_SCL GPIO_NUM_9
#define ADS1115_READ_INTERVAL_MS 30000
#define ADS1115_TOPIC "esp32/test/ads1115/ain0"
#define ADS1115_AIN1_TOPIC "esp32/test/ads1115/ain1"
#define ADS1115_RESISTANCE_TOPIC "esp32/test/ads1115/resistance"
#define PT1000_TEMPERATURE_TOPIC "esp32/test/pt1000/temperature"
#define R_NOTA_OHM 1000.0f
#define VCC_VOLTS 3.3f
#define PT1000_R0_OHM 1002.0f
#define PT1000_A 3.9083e-3f
#define PT1000_B (-5.775e-7f)
#define PT1000_C (-4.183e-12f)
#define ADS1115_FULL_SCALE_VOLTS 2.048f
#define DEFAULT_SLEEP_SECONDS (5ULL * 60ULL)
#define MIN_SLEEP_SECONDS 1ULL
#define MAX_SLEEP_SECONDS (24ULL * 60ULL * 60ULL)

static uint64_t sleep_seconds = DEFAULT_SLEEP_SECONDS;
static bool ads1115_ready = false;

static esp_err_t ads1115_write_register(uint8_t reg, uint16_t value)
{
    uint8_t data[] = {
        reg,
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xff)
    };
    return i2c_master_write_to_device(I2C_NUM_0, ADS1115_ADDR, data,
                                      sizeof(data), pdMS_TO_TICKS(100));
}

static esp_err_t ads1115_read_register(uint8_t reg, uint16_t *value)
{
    uint8_t data[2];
    esp_err_t err = i2c_master_write_read_device(
        I2C_NUM_0, ADS1115_ADDR, &reg, 1, data, sizeof(data),
        pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        *value = ((uint16_t)data[0] << 8) | data[1];
    }
    return err;
}

static esp_err_t ads1115_init(void)
{
    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = ADS1115_SDA,
        .scl_io_num = ADS1115_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &config));
    esp_err_t err = i2c_driver_install(I2C_NUM_0, config.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    uint16_t config_register;
    err = ads1115_read_register(0x01, &config_register);
    if (err == ESP_OK) {
        ads1115_ready = true;
        ESP_LOGI(TAG, "ADS1115 rilevato all'indirizzo 0x%02X", ADS1115_ADDR);
    } else {
        ESP_LOGW(TAG, "ADS1115 non rilevato: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t ads1115_read_channel(uint8_t channel, float *voltage)
{
    if (channel > 3) {
        return ESP_ERR_INVALID_ARG;
    }

    // AINx rispetto a GND, single-shot, gain +/-2.048 V, 128 SPS.
    uint16_t mux = 0x4000U + ((uint16_t)channel << 12);
    // OS=1, PGA=+/-2.048 V, single-shot, 128 SPS; MUX is added below.
    esp_err_t err = ads1115_write_register(0x01, 0x8583U | mux);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    uint16_t raw_register;
    err = ads1115_read_register(0x00, &raw_register);
    if (err == ESP_OK) {
        int16_t raw = (int16_t)raw_register;
        *voltage = (float)raw * ADS1115_FULL_SCALE_VOLTS / 32768.0f;
    }
    return err;
}

static bool pt1000_resistance_to_temperature(float resistance, float *temperature)
{
    if (!isfinite(resistance) || resistance <= 0.0f) {
        return false;
    }

    if (resistance >= PT1000_R0_OHM) {
        // For T >= 0 C, the Callendar-Van Dusen equation is quadratic.
        float discriminant = (PT1000_A * PT1000_A) -
                             (4.0f * PT1000_B *
                              (1.0f - resistance / PT1000_R0_OHM));
        if (discriminant < 0.0f) {
            return false;
        }
        *temperature = (-PT1000_A + sqrtf(discriminant)) /
                       (2.0f * PT1000_B);
        return isfinite(*temperature);
    }

    // For T < 0 C, solve the full equation, including coefficient C.
    float t = (resistance / PT1000_R0_OHM - 1.0f) / PT1000_A;
    for (int iteration = 0; iteration < 10; iteration++) {
        float t2 = t * t;
        float t3 = t2 * t;
        float equation = PT1000_R0_OHM *
                         (1.0f + PT1000_A * t + PT1000_B * t2 +
                          PT1000_C * (t - 100.0f) * t3) - resistance;
        float derivative = PT1000_R0_OHM *
                           (PT1000_A + 2.0f * PT1000_B * t +
                            PT1000_C * (4.0f * t3 - 300.0f * t2));
        if (fabsf(derivative) < 1e-9f) {
            return false;
        }
        float correction = equation / derivative;
        t -= correction;
        if (fabsf(correction) < 0.001f) {
            *temperature = t;
            return isfinite(t) && t >= -200.0f && t <= 0.0f;
        }
    }
    return false;
}

static void ads1115_task(void *arg)
{
    (void)arg;
    while (true) {
        if (ads1115_ready && mqtt_ok) {
            float voltage;
            esp_err_t err = ads1115_read_channel(0, &voltage);
            if (err == ESP_OK) {
                char voltage_payload[24];
                snprintf(voltage_payload, sizeof(voltage_payload), "%.4f",
                         (double)voltage);
                mqtt_manager_publish(ADS1115_TOPIC, voltage_payload, 1, false);

                if (voltage >= 0.0f && voltage < VCC_VOLTS) {
                    float r_incognita = R_NOTA_OHM * voltage /
                                        (VCC_VOLTS - voltage);
                    char resistance_payload[24];
                    snprintf(resistance_payload, sizeof(resistance_payload),
                             "%.2f", (double)r_incognita);
                    mqtt_manager_publish(ADS1115_RESISTANCE_TOPIC,
                                         resistance_payload, 1, false);

                    float temperature;
                    if (pt1000_resistance_to_temperature(r_incognita,
                                                          &temperature)) {
                        char temperature_payload[24];
                        snprintf(temperature_payload,
                                 sizeof(temperature_payload), "%.2f",
                                 (double)temperature);
                        mqtt_manager_publish(PT1000_TEMPERATURE_TOPIC,
                                             temperature_payload, 1, false);
                        ESP_LOGI(TAG, "ADS1115 AIN0: %s V, R: %s ohm, T: %s C",
                                 voltage_payload, resistance_payload,
                                 temperature_payload);
                    } else {
                        ESP_LOGW(TAG, "Resistenza PT1000 fuori intervallo: %s ohm",
                                 resistance_payload);
                    }
                } else {
                    ESP_LOGW(TAG, "Tensione ADS1115 non valida per il calcolo: %.4f V",
                             (double)voltage);
                }
            } else {
                ESP_LOGW(TAG, "Lettura ADS1115 fallita: %s",
                         esp_err_to_name(err));
            }

            float ain1_voltage;
            err = ads1115_read_channel(1, &ain1_voltage);
            if (err == ESP_OK) {
                char ain1_payload[24];
                snprintf(ain1_payload, sizeof(ain1_payload), "%.4f",
                         (double)ain1_voltage);
                mqtt_manager_publish(ADS1115_AIN1_TOPIC, ain1_payload, 1, false);
                ESP_LOGI(TAG, "ADS1115 AIN1: %s V", ain1_payload);
            } else {
                ESP_LOGW(TAG, "Lettura ADS1115 AIN1 fallita: %s",
                         esp_err_to_name(err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(ADS1115_READ_INTERVAL_MS));
    }
}

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
static bool mqtt_started = false;

static void publish_mqtt_status(void)
{
    if (mqtt_ok) {
        mqtt_manager_publish("esp32/test/status", "online", 1, true);
    }
}

static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "Deep sleep: risveglio tra %llu secondi",
             (unsigned long long)sleep_seconds);

    mqtt_ok = false;
    status_led_off();
    mqtt_manager_stop();
    esp_sleep_enable_timer_wakeup(sleep_seconds * 1000000ULL);
    esp_deep_sleep_start();
}

static void mqtt_status_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
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
        mqtt_manager_subscribe(MQTT_RESTART_TOPIC, 1);
    }
    update_led_state();
}
static void on_mqtt_message(const char *topic, const char *payload, int len)
{
    ESP_LOGI(TAG, "Messaggio ricevuto su '%s': %.*s", topic, len, payload);

    if (strcmp(topic, MQTT_RESTART_TOPIC) == 0) {
        if (len == (int)(sizeof(MQTT_RESTART_PAYLOAD) - 1) &&
            memcmp(payload, MQTT_RESTART_PAYLOAD, sizeof(MQTT_RESTART_PAYLOAD) - 1) == 0) {
            ESP_LOGW(TAG, "Riavvio richiesto via MQTT");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        } else {
            ESP_LOGW(TAG, "Comando restart non valido");
        }
        return;
    }

    if (strcmp(topic, MQTT_SLEEP_TOPIC) != 0) {
        return;
    }

    char command[32];
    if (len <= 0 || len >= (int)sizeof(command)) {
        ESP_LOGW(TAG, "Comando sleep non valido: payload troppo lungo");
        return;
    }

    memcpy(command, payload, (size_t)len);
    command[len] = '\0';

    if (strcmp(command, MQTT_SLEEP_PAYLOAD) == 0) {
        sleep_seconds = DEFAULT_SLEEP_SECONDS;
        enter_deep_sleep();
        return;
    }

    const char *seconds_text = command;
    if (strncmp(command, MQTT_SLEEP_PAYLOAD ":", sizeof(MQTT_SLEEP_PAYLOAD)) == 0) {
        seconds_text = command + sizeof(MQTT_SLEEP_PAYLOAD);
    }

    errno = 0;
    char *end = NULL;
    unsigned long long requested_seconds = strtoull(seconds_text, &end, 10);
    if (errno != 0 || end == seconds_text || *end != '\0' ||
        requested_seconds < MIN_SLEEP_SECONDS ||
        requested_seconds > MAX_SLEEP_SECONDS) {
        ESP_LOGW(TAG, "Tempo sleep non valido: '%s' (1-%llu secondi)",
                 command, (unsigned long long)MAX_SLEEP_SECONDS);
        return;
    }

    sleep_seconds = requested_seconds;
    enter_deep_sleep();
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
    ads1115_init();

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
    xTaskCreate(ads1115_task, "ads1115", 3072, NULL, 4, NULL);
    // Il check Internet parte comunque; se il WiFi non è connesso,
    // internet_check_now() fallirà semplicemente (timeout), senza crash
    internet_check_start(on_internet_status, 30000); // ogni 30 secondi
}