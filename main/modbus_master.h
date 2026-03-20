#ifndef MODBUS_MASTER_H
#define MODBUS_MASTER_H

#include <stdint.h>
#include "modbus_crc.h"

#define MB_SLAVE_MAX        8
#define MB_RETRY            3
#define MB_TIMEOUT_MS       200
#define MB_REG_MAX          8
#define MB_OFFLINE_ROUNDS   3   /* 连续 3 轮失败才判离线，单次超时不报警 */

typedef struct {
    uint8_t  addr;                      /* 从站地址 1~247 */
    uint16_t reg_start;                 /* 起始寄存器 */
    uint16_t reg_count;                 /* 寄存器数量 */
    uint16_t value[MB_REG_MAX];         /* 最近一次读回的值 */
    uint32_t seq;                       /* 成功读取的序号，用于判断数据是否刷新 */
    uint8_t  online;
    uint8_t  fail_rounds;               /* 连续失败轮数 */
    uint32_t err_cnt;                   /* 累计错误帧数，便于现场排查 */
} mb_slave_t;

void mb_init(void);
void mb_add_slave(uint8_t idx, uint8_t addr, uint16_t reg, uint16_t cnt);

/* 轮询一个从站，返回 0 成功，-1 失败。内部带重试，不阻塞其他从站的时隙。 */
int mb_poll_slave(uint8_t idx);

/* 供上报任务读取，避免直接摸内部数组 */
const mb_slave_t *mb_get_slave(uint8_t idx);
uint8_t mb_online_mask(void);

#endif
