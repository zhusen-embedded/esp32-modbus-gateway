# RS485/Modbus 边缘采集网关

ESP32-S3 当主站，轮询一串 RS485 从站，数据打 JSON 走 MQTT over TLS 发到云端。断网了先存 flash，网回来了按时间补传。

本来是想做个通用采集器，能接各种 Modbus 传感器，别每次换个现场就重新写一遍代码。

## 硬件

- ESP32-S3
- RS485 收发模块（MAX3485 那种自动方向的也行，我用的是带 DE 引脚的）
- 接线：UART1 TX=17 RX=18 DE=16，9600 8N1
- 终端电阻：线超过 10 米的话末端要补 120 Ω，不然误码率很高

## 现在能跑什么

- 最多挂 8 个从站，一个站 125 ms 时隙，8 个转一圈 1 秒
- 支持 0x03/0x04 读保持寄存器和输入寄存器
- 单站超时重试 3 次，连续 3 轮失败才判离线（避免偶发丢帧把好设备判死）
- 断网缓存到 flash，3072 条，1 秒一条大概能存 51 分钟
- 网恢复后自动补传，每轮限 32 条，不把新数据挤掉
- OTA 双分区，升级失败不会变砖
- 看门狗 5 秒，哪个任务卡死就重启

## 目录

```
main/
  app_main.c       四个任务 + 队列
  modbus_master.c  Modbus 主机（0x03/0x04、CRC16、异常码、重试）
  modbus_crc.c     CRC16，单独拆出来方便在 PC 上测
  mqtt_up.c        MQTT over TLS
  flash_ring.c     flash 环形缓存
  ota_client.c     OTA
  rs485.c          UART + DE 方向控制
test/
  test_host.c      纯逻辑单测，不用硬件
```

## 编译

```bash
idf.py set-target esp32s3
idf.py build
```

⚠️ 路径别带中文，原因见下面的坑。

## 单测

CRC 和环形缓存的计算不依赖 ESP-IDF，可以直接在 PC 上跑：

```bash
gcc -std=c11 -Wall -Wextra -I main -o test_host test/test_host.c main/modbus_crc.c
./test_host
```

跑的是 Modbus 规范原文里的向量 `11 03 00 6B 00 03 → 0x8776`，还有环形槽位回绕、连续 3 轮才离线的判定。

## 任务怎么分的

| 任务 | 优先级 | 干什么 |
|---|---|---|
| wdt | 6 | 每 10 秒打印一次健康状态 |
| poll | 5 | 轮询，结果丢进队列 |
| up | 3 | MQTT 上报 + 补传 |
| parse | 2 | 寄存器值换算成物理量 |

轮询优先级最高是因为它最讲究准时——8 个站靠时隙分，poll 被别的任务挤占了就会互相撞。上报慢一点没关系，MQTT 自己会排队。

`parse` 是我后来补的。原来只有三个任务，`app_main.c` 里 README 写了「四个任务」但代码里只有三个 `xTaskCreate`，自己看着别扭，就把解析拆出来了。拆出来还有个好处：poll 只负责收发，解析在另一个任务做，不会拖慢时隙。

## 踩坑记录

### DE 引脚拉低太早，最后一个字节被截断

RS485 半双工，发送和接收共用两根线，靠 DE 引脚切换方向。我一开始是这么写的：

```c
gpio_set_level(RS485_DE, 1);
uart_write_bytes(RS485_UART, data, len);
gpio_set_level(RS485_DE, 0);      // 立刻切回接收
```

`uart_write_bytes` 把数据塞进 FIFO 就返回了，这时候还在往外发。DE 一拉低，收发器立刻切到接收模式，最后那一两个字节根本没发出去。

现象是：从站有时候回，有时候不回，而且回的错帧居多。用逻辑分析仪抓了一下才看出来，从站收到的帧比发出去的短。

改成等发送完成：

```c
gpio_set_level(RS485_DE, 1);
uart_write_bytes(RS485_UART, data, len);
uart_wait_tx_done(RS485_UART, pdMS_TO_TICKS(100));   // 等移位寄存器真的空
gpio_set_level(RS485_DE, 0);
```

### 从站反复报离线，但设备是好的

日志里 slave 3 一直 online / offline 来回跳，去现场看那个设备好好的。

一开始以为是线的问题，量了通断、换了线，还是这样。后来抓波形发现偶尔整帧丢——不是 CRC 错，是根本没收到。

两个原因：

1. 现场那条线 30 米，末端没接终端电阻，信号反射严重
2. 我原来单次超时就置 offline，太敏感了

改法：

- 软件：连续 3 轮都失败才判离线
- 硬件：末端补 120 Ω

改完基本不误报了。

### 断网缓存不能用 NVS

第一版我用 NVS 存的，一条一个 key：

```c
snprintf(key, sizeof(key), "r%04u", s_head % RING_CAP);
nvs_set_str(h, key, buf);
```

跑了半天就出问题。三个毛病：

1. 默认 nvs 分区只有 24 KB，1 秒一条根本存不了几条
2. key 数量太多，NVS 的索引表撑不住
3. NVS 擦写是按页的，这么频繁写磨损很快

后来改成在独立的 storage 分区上自己划环形区：

- 每条记录 128 字节（16 字节头 + 112 字节 JSON）
- 4 KB 一个扇区，正好 32 条
- 384 KB 分区 = 3072 条，1 秒一条能存 51 分钟
- 整环按序号顺序推进，走到扇区边界才擦，擦除时机是确定的，不用 wear leveling

掉电一致性靠记录头里的 magic + seq + CRC16。上电时全分区扫一遍，把有效的找出来重建 head/tail。写了一半掉电的记录 CRC 过不了，自动跳过。

### 看门狗是假的

这个说出来有点丢人。原来 `wdt_task` 长这样：

```c
static void wdt_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

空转。既没订阅 TWDT，也没喂狗，更没有 `esp_task_wdt_add`。我当时的想法是「有个任务在那儿跑着就说明系统活着」，但这完全是自欺欺人——这个任务自己永远不会卡，卡的是别的任务。

后来改成每个任务自己喂：

```c
static void poll_task(void *arg) {
    while (1) {
        esp_task_wdt_reset();      // 我活着
        // ... 干活
    }
}
```

`wdt_task` 保留，但只负责打印健康状况（在线从站掩码、缓存条数、上报计数、OTA 状态），方便现场判断是谁卡了。

超时设 5 秒。轮询一帧最坏是 3 次重试 × 200 ms 超时 + 传输，大概 700 ms，留 5 秒余量足够。

### OTA 只写在 README 里

第一版 README 我写了「OTA 双分区，升级失败回滚」，但实际上代码里 `esp_ota` 一次都没出现过。就是想着「以后要加」，先写上去显得完整。

后来补 `ota_client.c` 的时候发现一个事：**「升级失败回滚」其实不用自己写**。`esp_ota_end()` 会校验镜像格式，格式不对就返回失败，这时候不调 `esp_ota_set_boot_partition()`，重启还是跑老固件——bootloader 看 otadata 里还是旧分区。

真正要自己处理的是另一件事：新镜像第一次启动时状态是 `ESP_OTA_IMG_PENDING_VERIFY`，如果不去 `mark_valid`，下次重启 bootloader 会认为这个镜像有问题，回滚回去。所以 `ota_init()` 里要先检查这个状态。

### Modbus 响应长度按寄存器数校验会越界

原来解析响应是这么写的：

```c
for (uint8_t i = 0; i < s->reg_count; i++) {
    s->value[i] = (buf[3 + i*2] << 8) | buf[4 + i*2];
}
```

循环次数用请求的寄存器数量。但如果从站回得比请求多（有些设备就是会多回），这里就冲到缓冲区外面去了。

改成先看响应里的字节数字段：

```c
uint8_t nbytes = buf[2];
if (nbytes & 1) return -1;                  // 寄存器是 16 位，字节数必须偶数
if (nbytes / 2 > s->reg_count) return -1;   // 回多了不采信
if (len < nbytes + 5) return -1;
```

### 编译路径不能有中文

和桌面时钟那个项目一样的坑。工程放在「找实习/简历/plans/」下面，编到 ldgen 那一步：

```
xtensa-esp32-elf-objdump.exe: .../libxtensa.a: No such file or directory
```

objdump 读不了中文路径。最后复制到 `C:\idfwork\` 编的。

## 还没做的

- **实测数据一个都没有**。8 从站 1 秒一轮是按配置算的，实际总线上的表现（误码率、丢包率、最坏响应时间）没测过。手上只有一个从站模块，凑不齐 8 个。
- **MQTT 那部分只验证了能编译**。broker 用的是示例地址，证书校验的代码写了但没接真服务器跑过。
- **OTA 没真升过级**。代码路径走通了，但没有第二个固件包去实测「升一半断电」这种场景。
- **parse 任务目前是空的**。现在只打了个 `ESP_LOGD`，真正的寄存器→物理量换算要根据具体传感器手册写。

## 下一步

- 凑齐几个不同型号的 Modbus 从站，实测一下轮询稳定性和误码率
- 找个能用的 MQTT broker（自己搭一个 mosquitto 也行），把 TLS 跑通
- 给 OTA 做个真的升级包，测一次完整流程
