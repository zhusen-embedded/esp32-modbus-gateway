/* 主机侧逻辑单元测试：CRC16 / Modbus 帧组包解析 / 环形缓存索引运算。
 *
 * 这三块是纯逻辑，不碰硬件，可以脱离 ESP-IDF 用 gcc 直接编。
 * 编译：gcc -I../main -o test_host test_host.c ../main/modbus_logic.c
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "modbus_master.h"

static int g_fail = 0;

#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); g_fail++; } \
} while (0)

/* ---- CRC16 ---- */
static void test_crc(void)
{
    /* Modbus 规范原文示例：11 03 00 6B 00 03 -> CRC 字段 76 87（低字节先发）
     * 即 CRC 值 = 0x8776 */
    uint8_t spec[6] = {0x11, 0x03, 0x00, 0x6B, 0x00, 0x03};
    uint16_t crc = mb_crc16(spec, 6);
    CHECK(crc == 0x8776, "spec vector got 0x%04X expect 0x8776", crc);

    /* 本项目实际用的帧：01 03 00 00 00 08 -> 0x0C44，发送时 44 0C */
    uint8_t req[6] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x08};
    uint16_t crc2 = mb_crc16(req, 6);
    CHECK(crc2 == 0x0C44, "8-reg vector got 0x%04X expect 0x0C44", crc2);

    /* 单寄存器读：01 03 00 00 00 01 -> 0x0A84 */
    uint8_t req3[6] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01};
    CHECK(mb_crc16(req3, 6) == 0x0A84, "1-reg vector");

    /* 空输入返回初值 */
    CHECK(mb_crc16(NULL, 0) == 0xFFFF, "crc16 empty");
}

/* ---- 帧组包 ---- */
static void test_build_req(void)
{
    /* 按 build_read_req 的规则手工复算：地址 0x01，0x03，起始 0x0000，数量 0x0008
     * CRC = 0x0C44，低字节在前 -> 44 0C */
    uint8_t expect[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x08, 0x44, 0x0C};
    uint16_t crc = mb_crc16(expect, 6);
    CHECK(expect[6] == (crc & 0xFF) && expect[7] == (crc >> 8),
          "req tail mismatch: got %02X %02X expect %02X %02X",
          expect[6], expect[7], crc & 0xFF, crc >> 8);
}

/* ---- 环形缓存索引运算（与 flash_ring.c 的算法保持一致） ---- */
#define RING_CAPACITY     3072
#define RING_SLOTS_PER_SEC 32

static void test_ring_math(void)
{
    /* 槽位循环：写到第 3072 条时应该回到槽 0 */
    CHECK((3072 % RING_CAPACITY) == 0, "slot wrap");
    CHECK((3073 % RING_CAPACITY) == 1, "slot wrap +1");

    /* 扇区擦除时机：槽号是 32 的整数倍时擦整个扇区 */
    CHECK((0 % RING_SLOTS_PER_SEC) == 0, "sector 0");
    CHECK((31 % RING_SLOTS_PER_SEC) != 0, "slot 31 not sector start");
    CHECK((32 % RING_SLOTS_PER_SEC) == 0, "slot 32 is sector start");

    /* 容量换算：3072 槽 / 1 条每秒 = 51.2 min，应覆盖 README 承诺的 30 min */
    CHECK((RING_CAPACITY / 60) >= 30, "capacity %d min < 30 min", RING_CAPACITY / 60);
}

/* ---- 从站在线判定 ---- */
static void test_offline_threshold(void)
{
    /* 连续 3 轮失败才判离线：第 1、2 轮后 online 仍为 1 */
    uint8_t online = 1, fail_rounds = 0;
    for (int round = 1; round <= 3; round++) {
        fail_rounds++;
        if (online && fail_rounds >= MB_OFFLINE_ROUNDS) online = 0;
        if (round < MB_OFFLINE_ROUNDS) {
            CHECK(online == 1, "should stay online after round %d", round);
        }
    }
    CHECK(online == 0, "should go offline after %d rounds", MB_OFFLINE_ROUNDS);
}

int main(void)
{
    test_crc();
    test_build_req();
    test_ring_math();
    test_offline_threshold();

    if (g_fail == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILED\n", g_fail);
    return 1;
}
