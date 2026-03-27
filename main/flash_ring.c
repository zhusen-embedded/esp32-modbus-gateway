#include "flash_ring.h"

#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "ring";

#define RING_MAGIC      0x52494E47u     /* "RING" */
#define SECTOR_SIZE     4096

typedef struct {
    uint32_t magic;
    uint32_t seq;          /* 单调递增，用于恢复顺序 */
    uint32_t ts;           /* 采集时刻（秒） */
    uint16_t len;          /* JSON 实际长度，含结尾 0 */
    uint16_t crc;          /* 对 seq/ts/len/json 算 CRC16 */
    char     json[RING_JSON_MAX];
} __attribute__((packed)) ring_rec_t;

_Static_assert(sizeof(ring_rec_t) == RING_SLOT_SIZE, "ring slot size mismatch");

static const esp_partition_t *s_part;
static uint32_t s_head;        /* 下一条要写入的 seq */
static uint32_t s_tail;        /* 最旧一条的 seq */
static uint32_t s_written;     /* 本次上电已写入条数，仅用于日志 */

static uint16_t crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) crc = (uint16_t)((crc >> 1) ^ 0xA001);
            else         crc >>= 1;
        }
    }
    return crc;
}

static uint16_t rec_crc(const ring_rec_t *r)
{
    /* 只校验会变的部分，magic 固定就不掺进来了 */
    return crc16((const uint8_t *)&r->seq,
                 sizeof(r->seq) + sizeof(r->ts) + sizeof(r->len) + r->len);
}

static esp_err_t read_slot(uint32_t seq, ring_rec_t *out)
{
    uint32_t slot = seq % RING_CAPACITY;
    return esp_partition_read(s_part, slot * RING_SLOT_SIZE, out, sizeof(*out));
}

static int rec_valid(const ring_rec_t *r)
{
    if (r->magic != RING_MAGIC) return 0;
    if (r->len == 0 || r->len > RING_JSON_MAX) return 0;
    if (r->json[r->len - 1] != '\0') return 0;
    return rec_crc(r) == r->crc;
}

/* 启动扫描：找到有效记录里最小的 seq 和最大的 seq，重建 tail/head。
 * 3072 槽 x 128 B = 384 KB 顺序读，分区是内存映射的，几十毫秒量级。 */
static void recover(void)
{
    ring_rec_t rec;
    uint32_t min_seq = UINT32_MAX, max_seq = 0;
    uint32_t valid = 0;

    for (uint32_t i = 0; i < RING_CAPACITY; i++) {
        if (esp_partition_read(s_part, i * RING_SLOT_SIZE, &rec, sizeof(rec)) != ESP_OK) {
            continue;
        }
        if (!rec_valid(&rec)) continue;
        if (rec.seq < min_seq) min_seq = rec.seq;
        if (rec.seq > max_seq) max_seq = rec.seq;
        valid++;
    }

    if (valid == 0) {
        s_head = 0;
        s_tail = 0;
    } else {
        s_tail = min_seq;
        s_head = max_seq + 1;
    }
    ESP_LOGI(TAG, "recover: %lu valid records, tail=%lu head=%lu",
             (unsigned long)valid, (unsigned long)s_tail, (unsigned long)s_head);
}

int ring_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (s_part == NULL) {
        ESP_LOGE(TAG, "partition 'storage' not found, check partitions.csv");
        return -1;
    }
    if (s_part->size < RING_CAPACITY * RING_SLOT_SIZE) {
        ESP_LOGE(TAG, "partition too small: %lu B", (unsigned long)s_part->size);
        return -1;
    }
    s_written = 0;
    recover();
    return 0;
}

int ring_push(const char *json, uint32_t ts)
{
    if (s_part == NULL) return -1;

    size_t len = strlen(json) + 1;
    if (len > RING_JSON_MAX) {
        ESP_LOGW(TAG, "record too long (%u), dropped", (unsigned)len);
        return -1;
    }

    uint32_t seq = s_head;
    uint32_t slot = seq % RING_CAPACITY;

    /* 写到一个扇区的第一个槽之前先擦整个扇区，
     * 否则旧数据（0x00）写不进去。整环按 seq 顺序推进，所以擦除时机是确定的。 */
    if ((slot % RING_SLOTS_PER_SEC) == 0) {
        esp_err_t err = esp_partition_erase_range(
            s_part, (slot / RING_SLOTS_PER_SEC) * SECTOR_SIZE, SECTOR_SIZE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "erase sector failed: %s", esp_err_to_name(err));
            return -1;
        }
    }

    ring_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = RING_MAGIC;
    rec.seq = seq;
    rec.ts = ts;
    rec.len = (uint16_t)len;
    memcpy(rec.json, json, len);
    rec.crc = rec_crc(&rec);

    esp_err_t err = esp_partition_write(s_part, slot * RING_SLOT_SIZE, &rec, sizeof(rec));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write slot %lu failed: %s", (unsigned long)slot, esp_err_to_name(err));
        return -1;
    }

    s_head++;
    s_written++;

    /* 满了丢最旧的 */
    if (s_head - s_tail > RING_CAPACITY) {
        s_tail = s_head - RING_CAPACITY;
        ESP_LOGW(TAG, "ring full, drop oldest (tail=%lu)", (unsigned long)s_tail);
    }
    return 0;
}

int ring_pop(char *out, size_t max, uint32_t *ts)
{
    if (s_part == NULL || s_tail >= s_head) return 0;

    ring_rec_t rec;
    if (read_slot(s_tail, &rec) != ESP_OK || !rec_valid(&rec)) {
        /* 坏记录（掉电写了半条）：跳过，不让它卡住整条补传链 */
        ESP_LOGW(TAG, "skip corrupt record seq=%lu", (unsigned long)s_tail);
        s_tail++;
        return 0;
    }

    strncpy(out, rec.json, max - 1);
    out[max - 1] = '\0';
    if (ts) *ts = rec.ts;

    s_tail++;
    return 1;
}

uint32_t ring_count(void)
{
    return s_head - s_tail;
}

uint32_t ring_capacity(void)
{
    return RING_CAPACITY;
}
