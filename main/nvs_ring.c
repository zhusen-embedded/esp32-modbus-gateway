#include "nvs_ring.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nvsring";
static nvs_handle_t s_h;
static uint32_t s_head, s_tail;

/* 环形区放在 NVS 的独立命名空间里，key 用 "r0000" 这种定长名，
 * 避免 key 太多把 NVS 索引撑爆。容量按 30 min × 1 条/秒 = 1800 条留余量，取 2048。
 */
#define RING_CAP    2048
#define NS_NAME     "ring"

int ring_init(void)
{
    esp_err_t err = nvs_open(NS_NAME, NVS_READWRITE, &s_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }
    nvs_get_u32(s_h, "head", &s_head);
    nvs_get_u32(s_h, "tail", &s_tail);
    ESP_LOGI(TAG, "ring init, head=%u tail=%u", s_head, s_tail);
    return 0;
}

int ring_push(const char *json, uint32_t ts)
{
    char key[8];
    char buf[192];

    /* 一条记录 = 时间戳 + 内容，用 '|' 分隔，读的时候再拆 */
    snprintf(buf, sizeof(buf), "%u|%s", ts, json);
    snprintf(key, sizeof(key), "r%04u", s_head % RING_CAP);

    esp_err_t err = nvs_set_str(s_h, key, buf);
    if (err != ESP_OK) return -1;

    s_head++;
    nvs_set_u32(s_h, "head", s_head);
    nvs_commit(s_h);

    /* 满了就丢最旧的：tail 往前推 */
    if (s_head - s_tail > RING_CAP) {
        s_tail = s_head - RING_CAP;
        nvs_set_u32(s_h, "tail", s_tail);
    }
    return 0;
}

int ring_pop(char *out, size_t max, uint32_t *ts)
{
    if (s_tail >= s_head) return 0;

    char key[8];
    char buf[192];
    size_t len = sizeof(buf);

    snprintf(key, sizeof(key), "r%04u", s_tail % RING_CAP);
    if (nvs_get_str(s_h, key, buf, &len) != ESP_OK) {
        /* 记录损坏，跳过 */
        s_tail++;
        return 0;
    }

    char *sep = strchr(buf, '|');
    if (!sep) { s_tail++; return 0; }

    *sep = '\0';
    *ts = (uint32_t)strtoul(buf, NULL, 10);
    strncpy(out, sep + 1, max - 1);
    out[max - 1] = '\0';

    s_tail++;
    nvs_set_u32(s_h, "tail", s_tail);
    nvs_commit(s_h);
    return 1;
}

uint32_t ring_count(void)
{
    return s_head - s_tail;
}
