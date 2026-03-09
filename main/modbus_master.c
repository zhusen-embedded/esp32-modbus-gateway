#include "modbus_master.h"
#include "rs485.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "modbus";
static mb_slave_t s_slaves[MB_SLAVE_MAX];
static uint8_t s_cnt;

/* Modbus CRC16：多项式 0xA001（反向），初值 0xFFFF，低字节在前 */
uint16_t mb_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else         crc >>= 1;
        }
    }
    return crc;
}

void mb_init(void)
{
    memset(s_slaves, 0, sizeof(s_slaves));
    s_cnt = 0;
    rs485_init();
}

void mb_add_slave(uint8_t idx, uint8_t addr, uint16_t reg, uint16_t cnt)
{
    if (idx >= MB_SLAVE_MAX) return;
    s_slaves[idx].addr = addr;
    s_slaves[idx].reg_start = reg;
    s_slaves[idx].reg_count = (cnt > 8) ? 8 : cnt;
    s_slaves[idx].online = 0;
    if (idx >= s_cnt) s_cnt = idx + 1;
}

/* 组一帧 0x03 请求：地址 功能码 起始高 起始低 数量高 数量低 CRC低 CRC高 */
static int build_read_req(uint8_t *out, const mb_slave_t *s)
{
    out[0] = s->addr;
    out[1] = 0x03;
    out[2] = (uint8_t)(s->reg_start >> 8);
    out[3] = (uint8_t)(s->reg_start & 0xFF);
    out[4] = (uint8_t)(s->reg_count >> 8);
    out[5] = (uint8_t)(s->reg_count & 0xFF);

    uint16_t crc = mb_crc16(out, 6);
    out[6] = (uint8_t)(crc & 0xFF);
    out[7] = (uint8_t)(crc >> 8);
    return 8;
}

/* 响应格式：地址 功能码 字节数 数据... CRC低 CRC高 */
static int parse_read_resp(const uint8_t *buf, int len, mb_slave_t *s)
{
    if (len < 5) return -1;
    if (buf[0] != s->addr || buf[1] != 0x03) return -1;

    uint8_t nbytes = buf[2];
    if (len < nbytes + 5) return -1;

    uint16_t crc = mb_crc16(buf, nbytes + 3);
    if (buf[nbytes + 3] != (uint8_t)(crc & 0xFF) ||
        buf[nbytes + 4] != (uint8_t)(crc >> 8)) {
        return -1;
    }

    for (uint8_t i = 0; i < s->reg_count; i++) {
        s->value[i] = ((uint16_t)buf[3 + i * 2] << 8) | buf[4 + i * 2];
    }
    return 0;
}

int mb_poll_slave(uint8_t idx)
{
    if (idx >= s_cnt) return -1;

    mb_slave_t *s = &s_slaves[idx];
    uint8_t req[8], resp[64];

    for (int attempt = 0; attempt < MB_RETRY; attempt++) {
        rs485_flush();
        int n = build_read_req(req, s);
        rs485_send(req, n);

        int got = rs485_recv(resp, sizeof(resp), MB_TIMEOUT_MS);
        if (got > 0 && parse_read_resp(resp, got, s) == 0) {
            if (!s->online) ESP_LOGI(TAG, "slave %d online", s->addr);
            s->online = 1;
            s->fail_cnt = 0;
            return 0;
        }
    }

    s->fail_cnt++;
    if (s->online) {
        ESP_LOGW(TAG, "slave %d offline after %d retries", s->addr, MB_RETRY);
        s->online = 0;
    }
    return -1;
}
