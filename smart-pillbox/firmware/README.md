# 智能药盒固件（ESP32-S3 / ESP-IDF 5.x）

多格（8 格）智能药盒固件：到点提醒 → 解锁指定药格 → 称重核验取药量 → 未取/少取重提醒/上报 →
低电量自动进入「收纳模式」（全部解锁，当普通药盒用）。

设计要点：**业务逻辑与硬件彻底分离**。`main/core/` 是不依赖任何 ESP-IDF 头文件的纯 C，
可以直接在 PC 上做单元测试；固件侧 `hal_esp.c` 只实现 `hal_if.h` 里的函数表（vtable）。

## 目录结构

```
firmware/
├── CMakeLists.txt          # IDF 工程入口（project(smart_pillbox)）
├── sdkconfig.defaults      # ESP32-S3 / 8MB QIO Flash / Octal PSRAM / NimBLE / MQTT / tickless
├── partitions.csv          # factory + ota_0 + ota_1 + nvs/otadata/phy/spiffs
├── main/
│   ├── app_main.c          # 主程序：初始化顺序、1Hz 主循环、APP 命令解析
│   ├── board_pins.h        # 全量引脚表
│   ├── hal_esp.c/.h        # pb_hal_t 的 ESP32-S3 实现 + 校时接口
│   ├── core/               # 纯 C 核心（状态机/称重判定/排程/库存）+ 主机测试共用
│   ├── services/
│   │   ├── net.c/.h        # Wi-Fi + MQTT（上下行、断线重连、遗嘱）
│   │   ├── ble.c/.h        # NimBLE 外设，向手环推送提醒（自定义服务 0xFFF0/0xFFF1）
│   │   ├── offline_queue.c/.h  # 断网事件 NVS 环形队列
│   │   └── ota.c/.h        # OTA 镜像确认（配合 otadata 回滚）
│   └── storage/
│       └── nvs_store.c/.h  # 药单/配置/配网信息持久化
└── test/host/              # 主机单元测试（gcc，无需 IDF）
```

## 1. 先跑主机测试（本地可验证）

```bat
cd firmware\test\host
build_run.bat
```

期望输出：`checks: 236, failed: 0` / `RESULT: ALL PASSED`。需要 `gcc` 在 PATH 中（MinGW/MSYS2 均可）。
这一步只编译 `main/core/*.c`，与固件构建互不影响。

## 2. 构建固件

ESP-IDF **5.1 ~ 5.4**（v5.x 任一版本，需支持 `esp_lcd` 与 NimBLE）：

```bash
# 一次性
. $HOME/esp/esp-idf/export.sh        # Windows: %IDF_PATH%\export.bat

cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

首次构建会自动按 `sdkconfig.defaults` 生成 `sdkconfig`。若之前已生成过 `sdkconfig`，
需要 `idf.py fullclean` 或删除 `sdkconfig` 后重新构建，`sdkconfig.defaults` 才会重新生效。

```bash
idf.py size          # 查看 flash/IRAM 占用
idf.py partition-table
```

### 组件依赖

`main/CMakeLists.txt` 已声明：`driver / esp_timer / esp_event / nvs_flash / esp_wifi /
esp_netif / mqtt / json / bt / esp_lcd / app_update`。全部属于 IDF 自带组件，无第三方依赖。

## 3. 引脚表（`main/board_pins.h`）

| 功能 | 引脚 | 说明 |
|---|---|---|
| I2C0 SDA / SCL | 1 / 2 | MAX17048(0x36) 电量计、DS3231(0x68) RTC，100 kHz |
| HX711 SCK | 4 | 8 片 HX711 共用时钟 |
| HX711 DOUT | 5,6,7,15,16,17,18,8 | 8 个药格各一路，10 Hz 采样 + 中值滤波 |
| 74HC595 DATA/CLK/LATCH | 9 / 10 / 11 | 16 bit：每格 2 bit 控制 H 桥正反转 |
| 电机总使能 | 12 | LEDC CH0，动作时满占空，静止关断 |
| 74HC165 DATA/CLK/LOAD | 13 / 14 / 21 | 16 bit：低 8 位开盖检测，高 8 位锁舌到位 |
| WT588D TX / BUSY | 43 / 44 | UART1 9600，BUSY 高表示正在播音 |
| ST7789 SCLK/MOSI/CS/DC/RST/BL | 47/48/38/39/40/41 | SPI2 20 MHz，背光 LEDC CH1 |
| 按键 OK / 静音 | 0 / 45 | 低有效，内部上拉（GPIO0 兼 BOOT） |
| RGB 状态灯 | 3 / 46 / 42 | 共阴，1 = 亮 |

GPIO26~37 被 Flash/Octal PSRAM 占用，表中已全部避开。**打样前请按最终原理图复核。**

## 4. 运行时线程模型

| 任务 | 优先级 | 职责 |
|---|---|---|
| `app_main`（主循环） | 1 | 每 1 s 调 `pb_core_tick()`；50 ms 轮询按键；消费 MQTT 下行命令；补发离线事件 |
| `pb_lock` | 6 | 串行执行开/关锁脉冲（450 ms 后断电，靠蜗轮自锁） |
| `pb_ui` | 2 | 500 ms 刷状态灯 / 背光 / 闪烁 |
| MQTT client | — | 收 `pillbox/<id>/cmd`，**只入队**，由主循环统一执行 |
| NimBLE host | — | 手环连接/订阅，通知推送 |

`core` 不是线程安全的，这一点由设计保证：所有状态变更只发生在主循环线程内
（MQTT 命令走 `net_take_command()` 队列，按键在主循环轮询，锁动作只发队列请求）。

## 5. MQTT 协议

设备 ID 形如 `pb-A1B2C3`（由 Wi-Fi MAC 后 3 字节生成，首次开机写入 NVS）。

- 上行：`pillbox/<id>/event`（QoS1，事件 JSON 由 core 生成，断网时进 NVS 环形队列，最多 8 条，满则丢最旧）
- 下行：`pillbox/<id>/cmd`
- 遗嘱：`{"type":"offline"}` 发布到 event 主题（retain）

### 下行命令

```json
{"cmd":"settime","ts":1735660800}
{"cmd":"setplan","cfg":{"tz_offset_s":28800,"noise_mg":150,"take_window_s":900},
 "slots":[
   {"name":"降压药","unit_mg":500,"stock":20,"tolerance_pct":15,
    "doses":[[480,1],[1200,1]]},
   {"name":"钙片","unit_mg":600,"stock":30,"at":"08:30","at_units":2}
 ]}
{"cmd":"unlock","slot":1}
{"cmd":"unlock_all"}
{"cmd":"ack"}                 // 确认已取药（等同按 OK 键）
{"cmd":"silence"}             // 静音（等同按静音键）
{"cmd":"storage"}             // 手动进入收纳模式
{"cmd":"recheck_battery"}
{"cmd":"wifi","ssid":"xxx","pass":"yyy"}   // 写入 NVS，重启生效
{"cmd":"reboot"}
```

- `slot` 为 **1..8**（对外 1 起，内部 0 起）。
- `doses` 支持 `[[分钟,片数], ...]`（分钟 = 本地 0..1439）或 `{"minute":480,"units":1}`；
  也可以用 `{"at":"08:30","at_units":1}` 单点写法，最多 4 条。
- 下发 `setplan` 会重建 core（cfg 在建核时拷贝），因此**会话进行中下发会打断当前提醒**，
  固件会打一条 `setplan during <state>, force rebuild` 的警告日志。
- `empty_mg` 默认 0：HX711 侧已在 `tare` 时做去皮，`read_mg()` 返回的是**相对空盒**的重量。

## 6. 掉电与重启行为

- 每 5 分钟把「库存 / 当天已触发位图 / 配置 / 收纳锁存」写入 NVS（`plan` blob）；
  修改药单、恢复收纳模式时立即落盘。
- 重启后按存盘内容恢复；若断电前已进入收纳模式（低电量），重启会直接恢复收纳模式，
  避免上锁后打不开。
- `otadata` 支持 OTA 回滚：`app_main` 在初始化成功后调用 `ota_boot_mark_valid()`，
  若新镜像起不来，bootloader 会自动退回上一版本。

## 7. 现场标定

1. 空盒装好，调用 `weight->tare(slot)`（可在串口控制台/调试命令里挂一个入口）；
2. 放入已知重量砝码，调用 `weight->calibrate(slot, known_mg)`，得到 `scale_x1000`；
3. 建议按 `test/host/test_dose_verify.c` 里的策略设置 `tolerance_pct`（默认 15%）与
   `noise_mg`（默认 150 mg），药片越轻容差越要放宽。

## 8. 已知限制

- 本仓库没有内置中文字库/LVGL：`display` 层只做**整屏底色 + 状态灯 + 背光**，
  屏上文字缓存在 `s_ui_title/s_ui_l1/s_ui_l2`，接 LVGL 时直接取用即可。
- WT588D 只按语句编号播报，不做在线 TTS。
- Wi-Fi 凭据变更需重启生效（`{"cmd":"wifi"}` 只写 NVS）。
- 未能在本机编译 ESP-IDF 工程（环境无 IDF 工具链），**固件层需在 IDF 5.x 下首次构建时复核**；
  纯 C 核心已由主机测试覆盖（236 项）。

