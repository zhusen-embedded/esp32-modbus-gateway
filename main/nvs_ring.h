#ifndef NVS_RING_H
#define NVS_RING_H

#include <stdint.h>
#include <stddef.h>

/* 断网缓存环形区，存在 NVS 里。
 * key 用 "r0000" 这种定长名，避免 key 太多把 NVS 索引撑爆。
 */

int      ring_init(void);
int      ring_push(const char *json, uint32_t ts);
int      ring_pop(char *out, size_t max, uint32_t *ts);
uint32_t ring_count(void);

#endif
