#include "modbus_master.h"
#include "rs485.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "modbus";
static mb_slave_t s_slaves[MB_SLAVE_MAX];
static uint8_t s_cnt;

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
    s_slaves[idx].reg_count = (cnt > MB_REG_MAX) ? MB_REG_MAX : cnt;
    s_slaves[idx].online = 0;
    s_slaves[idx].fail_rounds = 0;
    s_slaves[idx].err_cnt = 0;
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

/* 响应格式：地址 功能码 字节数 数据... CRC低 CRC高
 * 长度校验按「字节数」而不是寄存器数算，从站回 5 个寄存器也不会读越界。 */
static int parse_read_resp(const uint8_t *buf, int len, mb_slave_t *s)
{
    if (len < 5) return -1;
    if (buf[0] != s->addr || buf[1] != 0x03) return -1;

    uint8_t nbytes = buf[2];
    if (nbytes & 1) return -1;                  /* 寄存器是 16 位，字节数必须偶数 */
    if (nbytes / 2 > s->reg_count) return -1;   /* 从站回多了，不采信 */
    if (len < nbytes + 5) return -1;

    uint16_t crc = mb_crc16(buf, nbytes + 3);
    if (buf[nbytes + 3] != (uint8_t)(crc & 0xFF) ||
        buf[nbytes + 4] != (uint8_t)(crc >> 8)) {
        return -1;
    }

    uint8_t n = nbytes / 2;
    for (uint8_t i = 0; i < n; i++) {
        s->value[i] = ((uint16_t)buf[3 + i * 2] << 8) | buf[4 + i * 2];
    }
    return 0;
}

/* 异常响应（功能码最高位置 1）单独识别，便于现场区分「从站不存在」和「CRC 错」 */
static int is_exception_resp(const uint8_t *buf, int len, const mb_slave_t *s)
{
    return (len >= 5 && buf[0] == s->addr && (buf[1] & 0x80));
}

int mb_poll_slave(uint8_t idx)
{
    if (idx >= s_cnt) return -1;

    mb_slave_t *s = &s_slaves[idx];
    uint8_t req[8];
    uint8_t resp[64];

    for (int attempt = 0; attempt < MB_RETRY; attempt++) {
        rs485_flush();
        int n = build_read_req(req, s);
        rs485_send(req, n);

        int got = rs485_recv(resp, sizeof(resp), MB_TIMEOUT_MS);
        if (got > 0 && parse_read_resp(resp, got, s) == 0) {
            if (!s->online) {
                ESP_LOGI(TAG, "slave %d back online", s->addr);
            }
            s->online = 1;
            s->fail_rounds = 0;
            s->seq++;
            return 0;
        }

        if (got > 0 && is_exception_resp(resp, got, s)) {
            ESP_LOGW(TAG, "slave %d exception code 0x%02x", s->addr, resp[2]);
            break;              /* 异常响应重试没意义，直接记这一轮失败 */
        }
    }

    s->err_cnt++;
    if (s->fail_rounds < 255) s->fail_rounds++;

    /* 连续失败到阈值才翻 online，避免偶发丢帧把好设备判死 */
    if (s->online && s->fail_rounds >= MB_OFFLINE_ROUNDS) {
        ESP_LOGW(TAG, "slave %d offline after %d rounds (%lu errors)",
                 s->addr, s->fail_rounds, (unsigned long)s->err_cnt);
        s->online = 0;
    }
    return -1;
}

const mb_slave_t *mb_get_slave(uint8_t idx)
{
    if (idx >= s_cnt) return NULL;
    return &s_slaves[idx];
}

uint8_t mb_online_mask(void)
{
    uint8_t mask = 0;
    for (uint8_t i = 0; i < s_cnt; i++) {
        if (s_slaves[i].online) mask |= (uint8_t)(1u << i);
    }
    return mask;
}
