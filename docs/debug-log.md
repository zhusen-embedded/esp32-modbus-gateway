# 调试记录

按时间顺序记的，主要是 RS485 相关的。这部分坑最多，因为软件问题和硬件问题混在一起，很难分清是哪边。

---

## 从站反复报离线

**现象**

日志里 slave 3 一直 online / offline 来回跳：

```
I (12345) modbus: slave 3 online
W (12980) modbus: slave 3 offline
I (13610) modbus: slave 3 online
```

大概十几秒跳一次，但去现场看那个设备，指示灯是正常的，也在正常响应别的设备。

**排查**

第一步怀疑是线。现场那条 RS485 总线有 30 米左右，走了三个从站。用万用表量了通断，A/B 都没断，双绞也正常。

然后怀疑波特率。9600 是标准值，用示波器看了一下波形，边沿还算干净，不像速率不匹配。

抓包才看出问题：**偶尔整帧丢，不是 CRC 错**。如果是 CRC 错说明收到了但校验不过（一般是干扰），整帧丢说明根本没收到，是物理层的事。

**两个原因叠在一起**

1. 硬件：30 米线末端没接终端电阻。RS485 是差分总线，末端不匹配会有反射，信号叠在一起从站就认不出来。
2. 软件：我原来单次超时就置 offline：

```c
int mb_poll_slave(uint8_t idx) {
    for (int attempt = 0; attempt < MB_RETRY; attempt++) {
        // ... 发、收
    }
    s->online = 0;      // 三次都失败了就直接判死
    return -1;
}
```

3 次重试全失败就判离线，太敏感。总线上偶尔撞一次包很正常。

**改法**

软件上加了失败轮数计数，连续 3 轮（约 3 秒）都失败才翻 online：

```c
s->fail_rounds++;
if (s->online && s->fail_rounds >= MB_OFFLINE_ROUNDS) {
    ESP_LOGW(TAG, "slave %d offline after %d rounds", s->addr, s->fail_rounds);
    s->online = 0;
}

// 成功时清零
s->fail_rounds = 0;
```

硬件上在总线末端补了一个 120 Ω 电阻。

**结果**

误报基本消失了。偶尔还是会有单次失败，但不会再翻 online 状态。

**顺便**

后来查资料才知道，RS485 终端电阻不是「加了就好」——总线两端各加一个 120 Ω，中间的不加。我一开始在每段都加，反而把信号拉衰减了。

---

## 上电第一帧 CAN / Modbus 发不出去

**现象**

设备上电后第一帧必丢，从第二帧开始正常。日志上看不出来，因为发送函数返回成功。

**排查**

用逻辑分析仪抓 DE 引脚和 TX 波形。上电那一刻 DE 拉高了，但 TX 线上什么都没有。

**原因**

外设初始化时序。`uart_driver_install` 和 `uart_param_config` 返回成功，不代表 UART 真的准备好了，更不代表收发器供电稳定了。

RS485 收发器芯片上电到能工作有几十毫秒的建立时间。这段时间里 DE 拉高也没用，TX 上的数据出不去。

**改法**

初始化完加延时：

```c
rs485_init();
vTaskDelay(pdMS_TO_TICKS(50));    // 等收发器稳定
```

如果是 CAN 的话更麻烦，要等 `CAN_TSR` 的 TME 位：

```c
uint32_t t0 = HAL_GetTick();
while (!(CAN1->TSR & CAN_TSR_TME0)) {
    if (HAL_GetTick() - t0 > 10) return -1;
}
```

---

## DE 引脚切换太早，帧尾被吃掉

这个在 README 里也写了，这里记详细点。

**现象**

从站有时候回，有时候不回。回的时候也经常是错误响应。

**排查过程**

一开始以为是 CRC 算错了。拿逻辑分析仪抓 TX 线，把发出去的字节和预期的帧对比——发现发出去的帧**比预期的短**。

比如应该发 8 字节 `01 03 00 00 00 08 44 0C`，实际只发出了 6 个字节，最后两个 CRC 字节没了。

**原因**

```c
gpio_set_level(RS485_DE, 1);
uart_write_bytes(RS485_UART, data, len);   // 塞进 FIFO 就返回
gpio_set_level(RS485_DE, 0);               // 立刻切回接收
```

`uart_write_bytes` 是往 TX FIFO 里塞数据，塞完就返回了。这时候 FIFO 里的数据还在一位一位往外发（9600 波特率下 8 个字节约 8 ms）。DE 一拉低，收发器立刻切到接收模式，剩下没发完的字节就丢了。

9600 波特率下，8 字节要 8.3 ms，而 `uart_write_bytes` 返回大概只要几十微秒。差了三个数量级。

**改法**

```c
gpio_set_level(RS485_DE, 1);
uart_write_bytes(RS485_UART, data, len);
uart_wait_tx_done(RS485_UART, pdMS_TO_TICKS(100));   // 等移位寄存器空
gpio_set_level(RS485_DE, 0);
```

`uart_wait_tx_done` 会等到 FIFO 空**并且**移位寄存器也空，也就是最后一个 bit 真的出去了。

**顺便说个坑**

`uart_wait_tx_done` 和 `uart_flush` 不一样。`uart_flush` 是清空缓冲区（丢掉数据），`uart_wait_tx_done` 是等发送完成。我一开始看名字以为 `uart_flush` 是「把缓冲刷出去」，用了之后数据全丢了。

---

## NVS 写爆了

**现象**

跑了大半天之后，`nvs_set_str` 开始返回 `ESP_ERR_NVS_NOT_ENOUGH_SPACE`，缓存写不进去了。

**排查**

`nvs_get_stats` 看了一下，used_entries 满了。

**原因**

我的 key 是这么生成的：

```c
snprintf(key, sizeof(key), "r%04u", s_head % RING_CAP);
nvs_set_str(h, key, buf);
```

`RING_CAP` 设的 2048，也就是 2048 个不同的 key。但 NVS 一个命名空间的 entry 数量是有限的，而且每个 key 还要占索引空间。

更要命的是默认 `nvs` 分区只有 24 KB。1 秒一条、每条 JSON 一百多字节，24 KB 存不了几分钟就满了。

**改法**

换成独立分区的定长环形区，见 `flash_ring.c`：

- 记录固定 128 字节
- 4 KB 扇区 = 32 条
- 384 KB 分区 = 3072 条
- 走到扇区边界才擦，不用 NVS 的 GC

代价是不能用 NVS 那套 API 了，要自己管分区读写和 CRC。

**为什么不用 SPIFFS / LittleFS**

也考虑过。文件系统的问题是：每秒写一个小文件，会产生大量元数据更新，磨损比裸写分区严重得多，而且掉电时文件系统本身可能损坏（虽然 LittleFS 有掉电保护，但恢复要扫全盘）。

定长环形区最简单：位置算得出来，擦除时机确定，掉电最坏情况就是丢一条记录。

---

## OTA 升级后重启又回到老固件

**现象**

OTA 走完了，`esp_ota_set_boot_partition` 返回成功，重启后跑的却是老固件。

**排查**

`esp_ota_get_running_partition()` 打印出来是 `ota_0`，而我明明切到了 `ota_1`。

**原因**

新镜像第一次启动时状态是 `ESP_OTA_IMG_PENDING_VERIFY`。bootloader 看到这个状态，会认为「这个镜像还没被验证过」，如果不去确认，下次启动就回滚。

我一开始完全没处理这个状态。

**改法**

在 `ota_init()` 里检查：

```c
const esp_partition_t *running = esp_ota_get_running_partition();
esp_ota_img_states_t st;
if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
    }
}
```

**注意**

我现在是「能跑到 `ota_init` 就认为镜像没问题」，直接 mark valid。严格做法应该是先自检（读一次传感器、连一次网），确认功能正常再 mark。如果自检失败就不 mark，让 bootloader 回滚。

这个没做，因为现在的自检逻辑还没设计好——「功能正常」怎么定义？连不上网算不算失败？这些想清楚之前先简单处理。

---

## 看门狗从来没触发过

**现象**

不是 bug，是我发现原来那个「看门狗」根本不会工作。

**原来的代码**

```c
static void wdt_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

就一个空转循环。

**问题在哪**

1. 没有 `esp_task_wdt_add()` 订阅，TWDT 根本不知道有这个任务
2. 没有 `esp_task_wdt_reset()`，就算订阅了也会立刻超时
3. 其他任务也没订阅，所以任何一个卡死 TWDT 都不知道

**改法**

```c
static void wdt_register_all(void) {
    esp_task_wdt_config_t cfg = {
        .timeout_ms = 5000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err != ESP_OK) {
        ESP_ERROR_CHECK(esp_task_wdt_init(&cfg));
    }
}
```

每个任务里自己 reset：

```c
static void poll_task(void *arg) {
    while (1) {
        esp_task_wdt_reset();
        // ... 干活
    }
}
```

**为什么不用 `esp_task_wdt_add(NULL)` 统一管理**

那样的话所有任务共用一个超时，谁 reset 都算。等于只要有任何一个任务活着就不会触发，失去了意义。要各任务自己 reset 才能定位到具体是谁卡了。

---

## 待办

- 凑齐 8 个从站实测总线表现（误码率、最坏响应时间）
- 接真的 MQTT broker 跑 TLS
- 做第二个固件包测一次完整 OTA，包括「升级中途断电」
- `parse` 任务里的寄存器换算要按实际传感器手册写
