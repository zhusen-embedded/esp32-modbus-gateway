#include "mqtt_up.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client;
static volatile int s_connected;
static volatile int s_ready;
static char s_topic[96];
static uint32_t s_pub_ok, s_pub_fail;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = 1;
        ESP_LOGI(TAG, "connected, subscribe cmd");
        esp_mqtt_client_subscribe(s_client, "dev/cmd", 1);
        s_ready = 1;
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = 0;
        s_ready = 0;
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_DATA: {
        /* 云端下发命令：目前只处理 reboot，回执走 dev/<id>/ack */
        esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
        if (ev == NULL || ev->data == NULL) break;
        ESP_LOGI(TAG, "cmd topic=%.*s data=%.*s",
                 ev->topic_len, ev->topic, ev->data_len, ev->data);
        if (ev->data_len == 6 && memcmp(ev->data, "reboot", 6) == 0) {
            esp_mqtt_client_publish(s_client, "dev/cmd/ack", "rebooting", 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "mqtt error");
        break;
    default:
        break;
    }
}

void mqtt_up_init(const char *uri, const char *ca_pem)
{
    snprintf(s_topic, sizeof(s_topic), "dev/%s/data", CONFIG_GW_DEVICE_ID);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = CONFIG_GW_DEVICE_ID,
        .credentials.username = CONFIG_GW_MQTT_USER,
        /* 一定要校验证书，关掉校验等于把 TLS 白做了 */
        .broker.verification.certificate = ca_pem,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "client init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_mqtt_client_start(s_client);
}

int mqtt_up_publish_json(const char *json)
{
    if (!s_ready) {
        s_pub_fail++;
        return -1;
    }
    int msg_id = esp_mqtt_client_publish(s_client, s_topic, json, 0, 1, 0);
    if (msg_id < 0) {
        s_pub_fail++;
        return -1;
    }
    s_pub_ok++;
    return msg_id;
}

int mqtt_up_connected(void) { return s_connected; }
int mqtt_up_ready(void)     { return s_ready; }

uint32_t mqtt_up_pub_ok(void)   { return s_pub_ok; }
uint32_t mqtt_up_pub_fail(void) { return s_pub_fail; }
