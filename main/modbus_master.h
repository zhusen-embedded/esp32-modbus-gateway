#ifndef MODBUS_MASTER_H
#define MODBUS_MASTER_H

#include <stdint.h>

#define MB_SLAVE_MAX    8
#define MB_RETRY        3
#define MB_TIMEOUT_MS   200

typedef struct {
    uint8_t  addr;          /* 从站地址 1~247 */
    uint16_t reg_start;     /* 起始寄存器 */
    uint16_t reg_count;     /* 寄存器数量 */
    uint16_t value[8];      /* 读回的值 */
    uint8_t  online;
    uint8_t  fail_cnt;
} mb_slave_t;

void mb_init(void);
void mb_add_slave(uint8_t idx, uint8_t addr, uint16_t reg, uint16_t cnt);

/* 轮询一个从站，返回 0 成功，-1 失败。非阻塞，内部带重试。 */
int mb_poll_slave(uint8_t idx);

uint16_t mb_crc16(const uint8_t *buf, uint16_t len);

#endif
