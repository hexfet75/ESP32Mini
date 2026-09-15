#include "mqtt_manager.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include <string.h>

static const char *TAG = "mqtt_manager";
#define NVS_NAMESPACE "mqtt_cfg"

static esp_mqtt_client_handle_t client = NULL;
static mqtt_mgr_status_cb_t user_status_cb = NULL;
static mqtt_mgr_message_cb_t user_message_cb = NULL;

// ---------- Configurazione su NVS ----------

esp_err_t mqtt_manager_set_config(const char *broker_uri, const char *username, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "uri", broker_uri);
    if (err == ESP_OK) err = nvs_set_str(handle, "user", username);
    if (err == ESP_OK) err = nvs_set_str(handle, "pass", password);
    if (err == ESP_OK) err = nvs_commit(handle);

    nvs_close(handle);
    return err;
}

static esp_err_t load_config(char *uri, size_t uri_len, char *user, size_t user_len,
                              char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_str(handle, "uri", uri, &uri_len);
    if (err == ESP_OK) err = nvs_get_str(handle, "user", user, &user_len);
    if (err == ESP_OK) err = nvs_get_str(handle, "pass", pass, &pass_len);

    nvs_close(handle);
    return err;
}

static bool is_mqtts_uri(const char *uri)
{
    return strncmp(uri, "mqtts://", strlen("mqtts://")) == 0;
}

static bool is_mqtt_uri(const char *uri)
{
    return strncmp(uri, "mqtt://", strlen("mqtt://")) == 0;
}

// ---------- Event handler MQTT ----------

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connesso al broker MQTT");
            if (user_status_cb) user_status_cb(MQTT_MGR_CONNECTED);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnesso dal broker MQTT");
            if (user_status_cb) user_status_cb(MQTT_MGR_DISCONNECTED);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "Errore MQTT");
            if (user_status_cb) user_status_cb(MQTT_MGR_ERROR);
            break;

        case MQTT_EVENT_DATA:
            if (user_message_cb) {
                // Copie locali null-terminated per sicurezza (i buffer dell'evento non lo sono)
                char topic[128] = {0};
                char payload[512] = {0};
                int topic_len = event->topic_len < sizeof(topic) - 1 ? event->topic_len : sizeof(topic) - 1;
                int data_len = event->data_len < sizeof(payload) - 1 ? event->data_len : sizeof(payload) - 1;
                memcpy(topic, event->topic, topic_len);
                memcpy(payload, event->data, data_len);
                user_message_cb(topic, payload, data_len);
            }
            break;

        default:
            break;
    }
}

// ---------- API pubblica ----------

esp_err_t mqtt_manager_init(mqtt_mgr_status_cb_t status_cb, mqtt_mgr_message_cb_t message_cb)
{
    user_status_cb = status_cb;
    user_message_cb = message_cb;

    char uri[128] = {0}, username[64] = {0}, password[64] = {0};
    if (load_config(uri, sizeof(uri), username, sizeof(username), password, sizeof(password)) != ESP_OK) {
        ESP_LOGE(TAG, "Configurazione MQTT non trovata in NVS. Chiama mqtt_manager_set_config() prima.");
        return ESP_FAIL;
    }

    if (!is_mqtt_uri(uri) && !is_mqtts_uri(uri)) {
        ESP_LOGE(TAG, "URI MQTT non supportato: %s. Usa mqtt:// o mqtts://.", uri);
        return ESP_ERR_INVALID_ARG;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.username = username,
        .credentials.authentication.password = password,
    };

    if (is_mqtts_uri(uri)) {
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) return ESP_FAIL;

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    return ESP_OK;
}

esp_err_t mqtt_manager_start(void)
{
    if (!client) return ESP_FAIL;
    if (user_status_cb) user_status_cb(MQTT_MGR_CONNECTING);
    return esp_mqtt_client_start(client);
}

esp_err_t mqtt_manager_stop(void)
{
    if (!client) return ESP_FAIL;
    return esp_mqtt_client_stop(client);
}

esp_err_t mqtt_manager_publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (!client) return ESP_FAIL;
    int msg_id = esp_mqtt_client_publish(client, topic, payload, 0, qos, retain);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_manager_subscribe(const char *topic, int qos)
{
    if (!client) return ESP_FAIL;
    int msg_id = esp_mqtt_client_subscribe(client, topic, qos);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}