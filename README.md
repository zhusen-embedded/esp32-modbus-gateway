# RS485/Modbus 边缘采集网关

ESP32-S3 做主站，轮询 8 个 RS485 从站，数据经 MQTT over TLS 上报云端。断网时本地缓存，恢复后补传。

## 指标

| 项目 | 结果 |
|---|---|
| 从站数量 | 8 个，轮询周期 1 s |
| 断网缓存 | 30 min 无数据丢失 |
| 上报协议 | MQTT over TLS，JSON |
| OTA | 双分区，失败回滚 |

## 任务划分

| 任务 | 优先级 | 周期 | 职责 |
|---|---|---|---|
| wdt | 6 | 1 s | 喂狗 |
| poll | 5 | 125 ms/站 | Modbus 轮询 |
| up | 3 | 事件触发 | MQTT 上报 + 补传 |
| parse | 2 | 事件触发 | 协议解析 |

## 目录

```
main/
  app_main.c      任务创建
  modbus_master.c Modbus 主机状态机
  mqtt_up.c       MQTT 上报
  nvs_ring.c      断网缓存环形区
  rs485.c         UART1 + DE 方向控制
```

## 踩过的坑

1. **DE 引脚拉低太早**：发完立刻切回接收会截断最后一个字节，必须 `uart_wait_tx_done()` 等移位寄存器空
2. **从站误报离线**：单次超时就置 offline 太敏感，改成连续 3 轮失败才判；硬件上 30 m 线末端补 120 Ω 终端电阻
3. **NVS key 太多**：每秒一条、一条一个 key，很快把 NVS 索引撑爆，改成定长 key 的环形区
