#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "modbus_master.h"
#include "mqtt_up.h"
#include "nvs_ring.h"

static const char *TAG = "app";

/* 任务优先级：看门狗 > 轮询 > 上报 > 解析
 * 轮询必须准时，否则 8 个从站会互相挤占时隙；
 * 上报可以慢一点，MQTT 自己会排队。
 */
#define PRIO_WDT        6
#define PRIO_POLL       5
#define PRIO_UP         3
#define PRIO_PARSE      2

static EventGroupHandle_t s_evt;
#define BIT_NEW_DATA    (1 << 0)

/* 轮询任务：8 个从站轮流发，每个时隙一帧，单站超时不阻塞其他站 */
static void poll_task(void *arg)
{
    uint8_t idx = 0;
    TickType_t last = xTaskGetTickCount();

    while (1) {
        int ok = mb_poll_slave(idx);
        if (ok == 0) {
            xEventGroupSetBits(s_evt, BIT_NEW_DATA);
        }
        idx = (idx + 1) % MB_SLAVE_MAX;

        /* 8 个站 × 125 ms = 1 s 一轮 */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(125));
    }
}

/* 上报任务：有数据就发，发失败写缓存 */
static void up_task(void *arg)
{
    char json[160];
    char cached[160];
    uint32_t ts;

    while (1) {
        xEventGroupWaitBits(s_evt, BIT_NEW_DATA, pdTRUE, pdFALSE, portMAX_DELAY);

        snprintf(json, sizeof(json),
                 "{\"ts\":%lld,\"n\":%u}",
                 esp_timer_get_time() / 1000000, ring_count());

        if (mqtt_up_publish_json(json) != 0) {
            ring_push(json, (uint32_t)(esp_timer_get_time() / 1000000));
        }

        /* 网络恢复了先把积压的补传，限速避免把链路打满 */
        while (mqtt_up_connected() && ring_pop(cached, sizeof(cached), &ts)) {
            mqtt_up_publish_json(cached);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static void wdt_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    s_evt = xEventGroupCreate();

    mb_init();
    for (uint8_t i = 0; i < MB_SLAVE_MAX; i++) {
        mb_add_slave(i, (uint8_t)(i + 1), 0x0000, 4);
    }

    ring_init();
    mqtt_up_init(CONFIG_MQTT_URI, CONFIG_MQTT_CA);

    xTaskCreate(wdt_task,  "wdt",   2048, NULL, PRIO_WDT,   NULL);
    xTaskCreate(poll_task, "poll",  4096, NULL, PRIO_POLL,  NULL);
    xTaskCreate(up_task,   "up",    4096, NULL, PRIO_UP,    NULL);

    ESP_LOGI(TAG, "gateway started");
}
