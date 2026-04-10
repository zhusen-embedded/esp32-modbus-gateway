#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"

#include "modbus_master.h"
#include "mqtt_up.h"
#include "flash_ring.h"
#include "ota_client.h"

static const char *TAG = "app";

/* 任务优先级：看门狗 > 轮询 > 上报 > 解析
 * 轮询必须准时，否则 8 个从站会互相挤占时隙；
 * 上报可以慢一点，MQTT 自己会排队。
 */
#define PRIO_WDT        6
#define PRIO_POLL       5
#define PRIO_UP         3
#define PRIO_PARSE      2

#define STACK_WDT       2048
#define STACK_POLL      4096
#define STACK_UP        4096
#define STACK_PARSE     3072

/* 看门狗超时：轮询一帧最坏 = 3 次重试 x 200 ms 超时 + 传输 ≈ 700 ms，
 * 留 5 s 余量足够，真卡死也一定超时。 */
#define WDT_TIMEOUT_S   5

static EventGroupHandle_t s_evt;
#define BIT_NEW_DATA    (1 << 0)

/* 解析任务从轮询任务拿原始寄存器值，两个任务之间只传这一份快照，
 * 用队列而不是共享全局变量，省掉互斥量也避免了读到一半被改。 */
typedef struct {
    uint8_t  addr;
    uint16_t value[MB_REG_MAX];
    uint8_t  count;
    uint32_t seq;
} mb_sample_t;

static QueueHandle_t s_sample_q;

/* ---------------- 轮询任务 ---------------- */
static void poll_task(void *arg)
{
    uint8_t idx = 0;
    TickType_t last = xTaskGetTickCount();

    while (1) {
        /* 每轮都喂狗：这个任务是唯一可能被从站拖死的，它活着就说明总线没卡 */
        esp_task_wdt_reset();

        int ok = mb_poll_slave(idx);
        if (ok == 0) {
            const mb_slave_t *s = mb_get_slave(idx);
            mb_sample_t sample = {
                .addr = s->addr,
                .count = (uint8_t)s->reg_count,
                .seq = s->seq,
            };
            memcpy(sample.value, s->value, sizeof(sample.value));

            /* 队列满就丢最旧的，不能让轮询等上报 */
            if (xQueueSend(s_sample_q, &sample, 0) != pdTRUE) {
                mb_sample_t drop;
                xQueueReceive(s_sample_q, &drop, 0);
                xQueueSend(s_sample_q, &sample, 0);
                ESP_LOGW(TAG, "sample queue full, dropped slave %d", drop.addr);
            }
            xEventGroupSetBits(s_evt, BIT_NEW_DATA);
        }

        idx = (idx + 1) % CONFIG_GW_SLAVE_COUNT;

        /* 8 个站 × 125 ms = 1 s 一轮 */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(CONFIG_GW_POLL_SLOT_MS));
    }
}

/* ---------------- 解析任务 ---------------- */
static void parse_task(void *arg)
{
    mb_sample_t s;

    while (1) {
        if (xQueueReceive(s_sample_q, &s, portMAX_DELAY) != pdTRUE) continue;
        esp_task_wdt_reset();

        /* 现场传感器把寄存器当定点数用：温度 x10、湿度 x10、电压 mV。
         * 这里统一换算成物理量再拼 JSON，云端不用再懂 Modbus 编码。 */
        int16_t t10 = (int16_t)s.value[0];
        uint16_t rh10 = s.value[1];
        uint16_t mv = s.value[2];

        ESP_LOGD(TAG, "slave %d: %d.%d C, %u.%u %%, %u mV",
                 s.addr, t10 / 10, abs(t10 % 10), rh10 / 10, rh10 % 10, mv);
    }
}

/* ---------------- 上报任务 ---------------- */
static void up_task(void *arg)
{
    char json[160];
    char cached[RING_JSON_MAX];
    uint32_t ts;

    while (1) {
        xEventGroupWaitBits(s_evt, BIT_NEW_DATA, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        esp_task_wdt_reset();

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);

        snprintf(json, sizeof(json),
                 "{\"ts\":%" PRIu32 ",\"mask\":%u,\"q\":%" PRIu32 "}",
                 now, mb_online_mask(), ring_count());

        if (mqtt_up_publish_json(json) != 0) {
            ring_push(json, now);
        }

        /* 网络恢复了先把积压的补传，限速避免把链路打满 */
        int budget = 32;        /* 每轮最多补 32 条，防止补传把新数据挤掉 */
        while (mqtt_up_ready() && budget-- > 0 &&
               ring_pop(cached, sizeof(cached), &ts)) {
            if (mqtt_up_publish_json(cached) != 0) {
                /* 又断了，剩下的留到下一轮 */
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/* ---------------- 看门狗任务 ---------------- */
static void wdt_task(void *arg)
{
    while (1) {
        /* 真正喂狗的是各任务自己（poll/up/parse 里都有 esp_task_wdt_reset），
         * 这个任务只负责定期打印健康状况，方便现场判断是谁卡了。
         * 注意：它自己也要订阅 WDT，否则超时判定会漏掉它。 */
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "health: online=0x%02x ring=%" PRIu32 "/%" PRIu32
                      " pub_ok=%" PRIu32 " pub_fail=%" PRIu32 " ota=%s",
                 mb_online_mask(), ring_count(), ring_capacity(),
                 mqtt_up_pub_ok(), mqtt_up_pub_fail(), ota_state_str());
    }
}

static void wdt_register_all(void)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    /* IDF 5.x 里 TWDT 可能在别处已初始化，先试 reconfigure */
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err != ESP_OK) {
        ESP_ERROR_CHECK(esp_task_wdt_init(&cfg));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_evt = xEventGroupCreate();
    s_sample_q = xQueueCreate(16, sizeof(mb_sample_t));

    ota_init();

    mb_init();
    for (uint8_t i = 0; i < CONFIG_GW_SLAVE_COUNT; i++) {
        mb_add_slave(i, (uint8_t)(i + 1), 0x0000, CONFIG_GW_REG_COUNT);
    }

    if (ring_init() != 0) {
        ESP_LOGE(TAG, "offline buffer unavailable, continuing without it");
    }

    mqtt_up_init(CONFIG_GW_MQTT_URI, NULL);

    wdt_register_all();

    xTaskCreate(wdt_task,   "wdt",   STACK_WDT,   NULL, PRIO_WDT,   NULL);
    xTaskCreate(poll_task,  "poll",  STACK_POLL,  NULL, PRIO_POLL,  NULL);
    xTaskCreate(up_task,    "up",    STACK_UP,    NULL, PRIO_UP,    NULL);
    xTaskCreate(parse_task, "parse", STACK_PARSE, NULL, PRIO_PARSE, NULL);

    ESP_LOGI(TAG, "gateway started: %d slaves, %d regs, slot %d ms",
             CONFIG_GW_SLAVE_COUNT, CONFIG_GW_REG_COUNT, CONFIG_GW_POLL_SLOT_MS);

    /* OTA 由云端命令触发（dev/cmd 收到 "ota"），这里不自动跑，
     * 避免每次上电都去拉固件。 */
}
