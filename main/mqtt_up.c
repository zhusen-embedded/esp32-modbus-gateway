#include "mqtt_up.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include <string.h>

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client;
static volatile int s_connected;
static char s_topic[64];

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = 1;
        ESP_LOGI(TAG, "connected, subscribe cmd");
        esp_mqtt_client_subscribe(s_client, "dev/cmd", 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = 0;
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_DATA:
        /* 云端下发命令，目前只处理 reboot */
        break;
    default:
        break;
    }
}

void mqtt_up_init(const char *uri, const char *ca_pem)
{
    snprintf(s_topic, sizeof(s_topic), "dev/%s/data", CONFIG_DEVICE_ID);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = CONFIG_DEVICE_ID,
        /* 一定要校验证书，关掉校验等于把 TLS 白做了 */
        .broker.verification.certificate = ca_pem,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
    };

    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_mqtt_client_start(s_client);
}

int mqtt_up_publish_json(const char *json)
{
    if (!s_connected) return -1;
    return esp_mqtt_client_publish(s_client, s_topic, json, 0, 1, 0);
}

int mqtt_up_connected(void)
{
    return s_connected;
}
