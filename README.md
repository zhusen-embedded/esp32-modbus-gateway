# RS485/Modbus 边缘采集网关

ESP32-S3 做主站，轮询最多 8 个 RS485 从站，数据经 MQTT over TLS 上报云端。断网时写 flash 环形缓存，恢复后按时间戳补传。

> 状态：代码已完成并通过 `idf.py build`（ESP-IDF v5.5，esp32s3）。
> 下面「实测指标」一栏区分了两类数字：**代码可验证**（结构/容量/时序，由实现直接决定）
> 与 **硬件实测**（需要在真实总线上跑，标了「待实测」）。

## 指标

| 项目 | 结果 | 类型 |
|---|---|---|
| 从站数量 | 最多 8 个，单站时隙 125 ms，8 站 1 s 一轮 | 代码可验证（`CONFIG_GW_POLL_SLOT_MS`） |
| 断网缓存 | 3072 条定长记录，1 条/秒 → 约 51 min | 代码可验证（`RING_CAPACITY`） |
| 缓存掉电一致性 | 记录带 magic + seq + CRC16，上电全分区扫描重建 | 代码可验证（`flash_ring.c`） |
| 上报协议 | MQTT over TLS，JSON，按 `dev/{id}/data` 分主题 | 代码可验证 |
| OTA | 双分区，`esp_ota_end` 校验失败不切分区 | 代码可验证（`ota_client.c`） |
| 看门狗 | TWDT 5 s，poll/up/parse 各自喂狗，超时 panic | 代码可验证 |
| 单站超时重试 | 3 次 × 200 ms | 代码可验证（`MB_RETRY`/`MB_TIMEOUT_MS`） |
| 从站判离线 | 连续 3 轮失败才翻 online | 代码可验证（`MB_OFFLINE_ROUNDS`） |
| 误报离线率 / 丢包率 | 待实测 | 硬件实测 |

## 任务划分

| 任务 | 优先级 | 周期 | 职责 | 栈 |
|---|---|---|---|---|
| wdt | 6 | 10 s | 打印健康状况（online mask / ring / pub 计数 / ota 状态） | 2048 |
| poll | 5 | 125 ms/站 | Modbus 轮询，结果投队列 | 4096 |
| up | 3 | 事件触发 | MQTT 上报 + 断网补传（每轮限 32 条） | 4096 |
| parse | 2 | 事件触发 | 寄存器 → 物理量换算 | 3072 |

喂狗由各任务自己调 `esp_task_wdt_reset()`，不是靠一个空转任务——哪个任务卡死就一定会触发超时。

## 目录

```
main/
  app_main.c      四个任务 + 队列 + 事件组
  modbus_master.c Modbus 主机状态机（0x03/0x04、CRC16、异常码识别、重试）
  modbus_crc.c    CRC16，独立成文件以便 host 单测
  mqtt_up.c       MQTT over TLS，证书校验，发布计数
  flash_ring.c    flash 环形缓存（独立 storage 分区）
  ota_client.c    OTA：写备用分区 + 校验 + 切启动分区
  rs485.c         UART1 + DE 方向控制
test/
  test_host.c     CRC/帧/环形容量/离线阈值的 host 单测
partitions.csv    双 OTA + 384 KB storage
sdkconfig.defaults
```

## 编译与测试

```bash
# 固件（注意路径不要含中文，xtensa objdump 读不了）
idf.py set-target esp32s3
idf.py build

# 纯逻辑单测（不需要硬件）
gcc -std=c11 -Wall -Wextra -I main -o test_host test/test_host.c main/modbus_crc.c
./test_host
```

## 断网缓存为什么不用 NVS

1. 默认 `nvs` 分区只有 24 KB，装不下 1 条/秒 × 30 min；
2. 每秒一条、一条一个 key，NVS 索引和 GC 都撑不住；
3. NVS 擦写粒度是页，频繁 `set_str` 会放大磨损。

改成独立 `storage` 分区（384 KB）+ 128 B 定长槽位 + 32 槽/扇区。
整环按 seq 顺序推进，擦除时机确定（槽号是 32 的整数倍时擦整扇区），不需要 wear leveling。

## 踩过的坑

1. **DE 引脚拉低太早**：发完立刻切回接收会截断最后一个字节，必须 `uart_wait_tx_done()` 等移位寄存器空。
2. **从站误报离线**：单次超时就置 offline 太敏感，改成连续 3 轮失败才判；硬件上 30 m 线末端补 120 Ω 终端电阻。
3. **NVS key 太多**：每秒一条、一条一个 key，很快把 NVS 索引撑爆，改成独立分区的定长环形区。
4. **看门狗是假的**：最初 `wdt_task` 只是 `while(1){ vTaskDelay(1000); }`，根本没订阅 TWDT，卡死也不会重启。改成各任务自己 `esp_task_wdt_reset()`。
5. **OTA 只写了 README**：第一版仓库里 `esp_ota` 一次都没出现。补 `ota_client.c` 时发现 `esp_ota_end` 会校验镜像格式，所以「升级失败回滚」不需要自己写——校验没过就不切分区，重启还是老固件。
