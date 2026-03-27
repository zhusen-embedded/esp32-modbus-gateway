#ifndef OTA_CLIENT_H
#define OTA_CLIENT_H

#include <stdint.h>

typedef enum {
    OTA_IDLE = 0,
    OTA_RUNNING,
    OTA_DONE,       /* 已切分区，等待重启 */
    OTA_FAILED,
} ota_state_t;

void ota_init(void);

/* 有 url 才真的去下。返回 0 表示升级包已写入备用分区并切换启动分区，
 * 调用方负责重启。失败时正在运行的分区不受影响。 */
int  ota_start(const char *url);
ota_state_t ota_state(void);
const char *ota_state_str(void);

#endif
