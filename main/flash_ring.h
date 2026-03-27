#ifndef FLASH_RING_H
#define FLASH_RING_H

#include <stdint.h>
#include <stddef.h>

/* 断网缓存：直接读写 "storage" 数据分区的定长环形区。
 *
 * 为什么不用 NVS：
 *   1) 默认 nvs 分区只有 24 KB，装不下 30 min 的 1 条/秒；
 *   2) 每秒一条、一条一个 key，NVS 索引和 GC 都撑不住；
 *   3) NVS 擦写粒度是页，频繁 set_str 会放大磨损。
 *
 * 布局：槽位 128 B（16 B 头 + 112 B JSON），一个 4 KB 扇区正好 32 槽，
 *       384 KB 分区 = 3072 槽，1 条/秒可缓存约 51 min。
 * 掉电一致性：每条记录带 magic + seq + crc，启动时全分区扫描重建 head/tail，
 *       半条写坏（掉电中断）的记录 crc 校验不过，自动跳过。
 */

#define RING_SLOT_SIZE      128
#define RING_JSON_MAX       112
#define RING_SLOTS_PER_SEC  32      /* 4096 / 128 */
#define RING_CAPACITY       3072

int      ring_init(void);
int      ring_push(const char *json, uint32_t ts);
int      ring_pop(char *out, size_t max, uint32_t *ts);
uint32_t ring_count(void);
uint32_t ring_capacity(void);

#endif
