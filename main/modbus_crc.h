#ifndef MODBUS_CRC_H
#define MODBUS_CRC_H

#include <stdint.h>

/* Modbus CRC16：多项式 0xA001（反向），初值 0xFFFF。
 * 单独成文件是为了能脱离 ESP-IDF 用 host gcc 跑单元测试。 */
uint16_t mb_crc16(const uint8_t *buf, uint16_t len);

#endif
