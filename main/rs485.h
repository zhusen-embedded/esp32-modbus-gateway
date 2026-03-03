#ifndef RS485_H
#define RS485_H

#include <stdint.h>
#include <stddef.h>

#define RS485_UART      UART_NUM_1
#define RS485_TXD       17
#define RS485_RXD       18
#define RS485_DE        16      /* 方向控制：高发送，低接收 */
#define RS485_BAUD      9600

void rs485_init(void);
int  rs485_send(const uint8_t *data, size_t len);
int  rs485_recv(uint8_t *buf, size_t max, int timeout_ms);
void rs485_flush(void);

#endif
