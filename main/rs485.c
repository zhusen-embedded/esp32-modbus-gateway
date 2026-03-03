#include "rs485.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "rs485";

void rs485_init(void)
{
    uart_config_t cfg = {
        .baud_rate = RS485_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_install(RS485_UART, 512, 512, 0, NULL, 0);
    uart_param_config(RS485_UART, &cfg);
    uart_set_pin(RS485_UART, RS485_TXD, RS485_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    /* DE 引脚手动控制：发完立刻拉低会截断最后一个字节，
     * 要等 UART 移位寄存器真正空掉，用 uart_wait_tx_done 保证。 */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << RS485_DE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(RS485_DE, 0);

    ESP_LOGI(TAG, "uart1 ready, %d 8N1, DE=GPIO%d", RS485_BAUD, RS485_DE);
}

int rs485_send(const uint8_t *data, size_t len)
{
    gpio_set_level(RS485_DE, 1);
    int n = uart_write_bytes(RS485_UART, data, len);
    /* 等最后一位移出去再切回接收，否则从站收到的是残帧 */
    uart_wait_tx_done(RS485_UART, pdMS_TO_TICKS(100));
    gpio_set_level(RS485_DE, 0);
    return n;
}

int rs485_recv(uint8_t *buf, size_t max, int timeout_ms)
{
    return uart_read_bytes(RS485_UART, buf, max, pdMS_TO_TICKS(timeout_ms));
}

void rs485_flush(void)
{
    uart_flush_input(RS485_UART);
}
