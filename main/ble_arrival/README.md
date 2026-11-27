# 门口迎宾模块（BLE 接近检测）

在 `78/xiaozhi-esp32` v2.6.0 之上新增的扩展应用：**检测到指定 BLE 信标靠近时，在屏幕上显示文字并播放提示音**。

设备端完全自治——不依赖服务端 `notify`、不依赖 PC、不依赖手机。只要信标在附近，设备自己判断、自己播报。

---

## 1. 目录

| 文件 | 语言 | 作用 |
|---|---|---|
| `arrival_detector.h` / `.c` | **纯 C99** | 到达判定核心：中值滤波 → 滑动平均 → 迟滞双阈值 → 连续命中 → 冷却 → 事件。无 IDF 依赖、无 STL、无动态内存，可在主机侧单独编译测试 |
| `arrival_target.h` / `.cc` | C++ | 信标目标：MAC / iBeacon UUID 双模式匹配、用户输入解析、NVS 持久化、AD 结构里的 iBeacon 帧解析 |
| `ble_arrival_monitor.h` / `.cc` | C++ | BLE 扫描（Bluedroid）、扫描结果队列、检测任务、候选设备发现、忙碌时暂停扫描 |
| `ble_arrival_web.h` / `.cc` | C++ | 固件自起的极简 httpd：状态页、绑定/解绑、在线调阈值 |

配套接入改动（不在本目录）：

| 文件 | 改动 |
|---|---|
| `main/CMakeLists.txt` | `CONFIG_BLE_ARRIVAL_ENABLED` 打开时才编入本目录，并追加 `esp_http_server` / `esp_wifi` 依赖 |
| `main/Kconfig.projbuild` | 新增菜单 `BLE Arrival Sensor (门口迎宾)`，全部参数可配 |
| `main/application.cc` | `Initialize()` 里启动监控；播报用 `Schedule()` 转到主任务执行 `Alert()` |
| `main/boards/lckfb/szpi-esp32s3/config.json` | 新增变体 `lichuang-dev-ble`（打开 BLE 栈 + 本功能） |

---

## 2. 数据流

```
射频
 │
 ▼
Bluedroid GAP 回调（蓝牙任务）        ← 只做「解析 + 入队」，绝不做耗时操作
 │  ScanEntry{ mac, rssi, uuid?, major?, minor?, ts_ms }
 ▼
FreeRTOS 队列（64 条，满了丢包计数）
 │
 ▼
检测任务（100ms tick）
 │  · 匹配目标（MAC 或 iBeacon）
 │  · arrival_detector_feed(rssi, ts)
 │  · 产生 ARRIVED / LEFT 事件
 ▼
播报回调 → Application::Schedule() → 主任务 → Alert(屏幕文字 + OGG_SUCCESS/OGG_VIBRATION)
```

---

## 3. 绑定手段（三层优先级）

| 优先级 | 来源 | 生效时机 | 适用场景 |
|---|---|---|---|
| 1 | **运行时 NVS** | Web 页面绑定后立即生效，重启保留 | 日常使用，随时改信标 |
| 2 | **编译期 Kconfig** `CONFIG_BLE_ARRIVAL_TARGET_MAC` | 每次启动 | 一次绑定永久生效，零交互 |
| 3 | **自动发现**（未绑定） | 启动即进入 | 先观察环境里都有谁，再决定绑哪个 |

### 3.1 支持的目标格式

```
AA:BB:CC:DD:EE:FF                              MAC 模式（推荐）
E2C56DB5-DFFB-48D2-B060-D0F5A71096E0           iBeacon UUID 模式
E2C56DB5-DFFB-48D2-B060-D0F5A71096E0:100:200    UUID + major + minor
E2C56DB5DFFB48D2B060D0F5A71096E0:100:200        同上，无连字符
```

major/minor 支持十进制与 `0x` 十六进制。

### 3.2 怎么绑

**方式 A：Web 页面（推荐）**

设备联网后用手机浏览器打开 `http://<设备IP>:8080/`：

- 看实时 RSSI、滤波值、事件计数、丢包数
- 未绑定时页面下方会列出**候选设备**（按广播次数排序），把 MAC 填进输入框即可绑定
- 在线调「进入阈值 / 迟滞」，改完立刻生效并写入 NVS

> ⚠️ 这个 httpd **没有鉴权**。任何能连到设备的人都能改绑定和阈值。不要做端口映射，不要暴露到公网。

**方式 B：编译期写死**

在 `main/boards/lckfb/szpi-esp32s3/config.json` 的 `lichuang-dev-ble` 变体里加：

```json
"CONFIG_BLE_ARRIVAL_TARGET_MAC=AA:BB:CC:DD:EE:FF"
```

**方式 C：串口日志里的候选提示**

未绑定时，设备每 10 秒会在串口打印一次最强候选设备的 MAC，方便在无网环境下手工确认。

---

## 4. 参数标定手册

**绝对 RSSI 阈值不可跨设备移植**——PC 上测的值不能用在 ESP32-S3 上（天线增益、位置、供电都不同）。必须在本设备上标定。

### 4.1 标定步骤

1. 烧上固件，手机连同一个 Wi-Fi，打开 `http://<设备IP>:8080/`
2. **信标拿在门口 1 米处**，看页面上的「滤波 RSSI」，记作 `RSSI_near`
3. **退到楼下/走廊尽头**，同样记下 `RSSI_far`
4. 进入阈值取两者之间、偏门口一侧：

   ```
   enter_threshold = RSSI_far + 0.6 × (RSSI_near - RSSI_far)
   ```

   例：`RSSI_far = -88`，`RSSI_near = -58` → `enter = -88 + 0.6×30 = -70`
5. 迟滞取 `10~14 dB`（默认 12），退出阈值 = 进入阈值 - 迟滞
6. 在页面里填入并保存 → 写入 NVS，重启仍然有效

### 4.2 各参数含义

| 参数 | 默认 | 说明 |
|---|---|---|
| `scan_interval` / `scan_window` | 100ms / 50ms | 占空比 50%。ESP32-S3 与 Wi-Fi 分时复用，占空比别超过 50% |
| `median_window` | 5 | 中值滤波，去单点毛刺 |
| `avg_window` | 10 | 滑动平均，平滑残余抖动 |
| `enter_threshold` | -70 | 进入阈值（**必须标定**） |
| `hysteresis_db` | 12 | 迟滞带宽，避免边界反复横跳 |
| `hit_count` | 3 | 连续几次超阈值才算到达 |
| `miss_count` | 4 | 连续几次低于退出阈值才算离开 |
| `cooldown_ms` | 60000 | 两次到达播报最小间隔 |
| `stale_timeout_ms` | 8000 | 多久没收到广播算离开 |
| `tick_ms` | 100 | 检测任务周期 |

### 4.3 预期时延（合成走位数据集实测）

用 `test/ble_arrival/data/synthetic-walk.csv`（噪声特征取自实测）回放：

| 事件 | 真值时刻 | 判定时刻 | 延迟 |
|---|---|---|---|
| 到达 | 30.0s | 31.6s | **+1.6s** |
| 离开 | 72.0s | 74.2s | **+2.2s** |

远场（20s）无任何误报。

### 4.4 想要更快响应

调小 `avg_window`（10→5）和 `hit_count`（3→2），代价是抖动增加、误报变多。

---

## 5. 内存与射频开销

### 5.1 基线：官方 v2.6.0 固件的实测余量

数据来自 `../../../../firmware/boot-com8.log`（本机实跑，不是估算）：

| 项 | 实测值 | 来源 |
|---|---|---|
| 内部 RAM 动态堆池 | **约 300 KiB**（240 + 21 + 32 DRAM + 7 RTCRAM） | `heap_init` 四行 |
| 其中预留 DMA/内部分配 | 100 KiB | `esp_psram: Reserving pool of 100K of internal memory for DMA/internal allocations` |
| **内部 SRAM 空闲** | **175 239 B ≈ 171 KB** | `SystemInfo: free sram: 175239` |
| **内部 SRAM 历史最低水位** | **73 712 B ≈ 72 KB** | `minimal sram: 73712` |
| PSRAM | 8 MB 全量入堆 | `Adding pool of 8192K of PSRAM memory` |
| 已在 PSRAM 的大件 | LVGL 池 2560 KB、图片缓存 2 MB、摄像头帧缓冲 153 600 B | boot log |

**关键结论**：语音助手的大头（LVGL、模型、图像、摄像头缓冲）**本来就在 PSRAM 里**，吃内部 SRAM 的主要是 Wi-Fi + 音频 + 网络栈。所以内部 SRAM 余量是唯一的硬约束。

> 水位线的含义：整个开机过程里内部 SRAM 最低只到过 72 KB，也就是**离"申请失败"最近时还差 72 KB**。上面还有 99 KB 的头顶空间。

### 5.2 新增开销

| 项 | 开销 | 说明 |
|---|---|---|
| Bluedroid 主机栈 + 控制器 | **待实测，预算 100~140 KB** | 唯一的大头。乐鑫官方文档实测：ESP32-S3 上 Wi-Fi Station 空余 273 K，加上 BLE 共存示例后剩 189 K → **BLE 增量约 84 KB**（该示例基于 NimBLE，Bluedroid 更重） |
| 检测任务栈 | 4 KB | 优先级 3，低于 audio_output(4) |
| 扫描队列 | 64 × 32 B ≈ 2 KB | 堆 |
| Web 任务栈 | 4 KB | 堆，可关 |
| 其余（互斥量、候选表、NVS） | < 1 KB | |

`BT_CLASSIC_ENABLED` 默认 n（只开 BLE），`BT_CTRL_BLE_MAX_ACT` 默认 6（每个实例 828 B，只扫描用 2~3 即可）。

### 5.3 怎么实测（刷完固件就能看）

新固件在蓝牙栈初始化**前后各打一次**内部 SRAM 余量，再叠加官方固件本来就有每 10 秒一次的 `free sram`：

```
I (xxxx) BleArrival: 蓝牙栈初始化前: 内部SRAM free=175239 minimal=73712
I (xxxx) BleArrival: 蓝牙栈初始化后: 内部SRAM free=xxxxx minimal=xxxxx
```

官方 v2.6.0 的对照值是 **free=175239 / minimal=73712**。差值就是蓝牙栈的真实吃法。

### 5.4 射频共存

ESP32-S3 只有一颗 2.4GHz 射频，Wi-Fi 与 BLE 分时复用。官方兼容矩阵里「BLE Scan + Wi-Fi STA 已连接」是 **C1（可用但性能不稳定）**。本模块的对策：

1. 短占空比扫描（50ms / 100ms）
2. **设备在收音/说话/播报时自动暂停扫描**，把射频让给 Wi-Fi 音频通道
3. 软件共存（`CONFIG_ESP_COEX_SW_COEXIST_ENABLE`）在 BT 打开时默认自动开启
4. 蓝牙任务优先级压到 3，低于 audio_output(4) / opus(7) / audio_input(8) / 主任务(10)

如果发现语音助手在说话时卡顿，把 `scan_window` 降到 25ms（25% 占空比）。

### 5.5 内存不够时的降级阶梯（按代价从低到高）

| 级别 | 动作 | 省多少 | 代价 |
|---|---|---|---|
| 1 | `CONFIG_BLE_ARRIVAL_WEB_ENABLED=n` | 约 6~8 KB | 失去网页绑定/调阈值，只能用编译期或串口 |
| 2 | `CONFIG_BT_LOG_CRITICAL_ONLY_HOST=y` + `..._CTRL=y` | 若干 KB DRAM | 蓝牙日志变少 |
| 3 | `CONFIG_BT_CTRL_BLE_MAX_ACT=2` | 约 3 KB | 只够扫描 |
| 4 | `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y` | **20 KB+**（控制器代码从 IRAM 挪到 flash） | 蓝牙性能下降，flash 擦写时可能不稳（需开 flash 自动 suspend） |
| 5 | 改用 NimBLE 主机栈 | 约 50 KB | 工作量集中在 `ble_arrival_monitor.cc` 一个文件的扫描层重写 |
| 6 | 放弃 A1，改门磁/按键 | — | 失去"人到了"这个语义 |

第 4 级是官方 Kconfig 里明确写着 "saving over 20K bytes of memory" 的开关，是性价比最高的一档。

---

## 6. 已知坑（都踩过）

1. **`esp_ble_gap_set_scan_params()` 必须在 `esp_ble_gap_register_callback()` 之后调用**，否则参数设置无效。
2. **GAP 回调里绝不能做耗时操作**（打日志、碰显示、碰音频都不行）。回调跑在蓝牙栈任务里，阻塞它会拖垮整个蓝牙栈。本模块的回调只做「解析 + 入队」。
3. **必须 `scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE`**。控制器默认会过滤重复广播，只报「每个设备一条」；而接近判定需要的是**同一个信标的连续 RSSI 样本**。
4. **`esp_ble_scan_params_t` 在 IDF v6.1 里多了 `own_addr_type` 字段**，少填会编译失败。
5. **v2.6.0 官方固件里完全没有蓝牙协议栈**（`esp_bluedroid_init` / `esp_ble_gap_*` 全部 not found），要开蓝牙必须自己编固件。
6. **`bt` 组件本来就在 `main` 的 PRIV_REQUIRES 里**，但 `CONFIG_BT_ENABLED` 默认是 n，所以不额外开配置的话链接进去的只是一个空壳。
7. **ESP32-S3 / C3 的控制器没有 BR/EDR 模式选项**（Kconfig 里只 source 了 esp32c3 那份），所以 `BT_BLE_ENABLED` 默认就是 y，不用手动设。
8. **中文播报文字要能显示出来**，依赖字体包 `font_noto_sans_common_*.bin`（与语言无关，common 字符集），zh-CN 设备本来就在用它显示中文，所以没问题。

---

## 7. 编译

```bash
# 需要 ESP-IDF v6.1 环境
cd src/xiaozhi-esp32
python scripts/build.py lckfb/szpi-esp32s3 --name lichuang-dev-ble
# 产物: build/merged-binary.bin
```

> 注意板型参数是**相对路径** `lckfb/szpi-esp32s3`，不是 `lckfb-lichuang-dev`（后者是旧写法）。

GitHub Actions 已配好 `.github/workflows/ble-arrival.yml`：
- `host-test` job：在普通 Linux runner 上编译并运行到达判定单元测试，并校验金标准向量可复现
- `idf-build` job：用 `espressif/idf:release-v6.1` 容器编译固件，产出 artifact

---

## 8. 信标怎么买（2026-11-23 已核实厂商规格）

**接收端零额外硬件**——ESP32-S3 自带 BLE 5.0 和天线。

### 8.1 结论：买 EW-BCN05L，广播间隔设 **200ms**

| | **EW-BCN05L**（推荐） | EW-BCN05 | VDB03S |
|---|---|---|---|
| 厂商 | 研为通讯 easywphy | 研为通讯 easywphy | 微能信息 95Power |
| 芯片 | Nordic nRF52832 | Nordic nRF52832 | BLE 4.2 |
| 尺寸 | 46×41×6mm / 7g | 46×41×6mm / 7g | 37.3×23.4×5.1mm |
| 发射功率 | **-19.5~+2.5 dBm**（默认 +2.5） | -40~**+4 dBm**（默认 +4） | -20~**0 dBm** |
| 平均电流 | **10 uA**（@+2.5dBm/1s） | 32 uA（@+4dBm/1s） | — |
| 广播间隔 | 100ms~10s（默认 1s） | 100ms~10s（默认 1s） | 可配，**但改完会丢** |
| 续航（@1s） | **2 年以上** | 9 个月 | 19 个月 |
| 广播距离 | 60 m | 60~100 m | 50 m |
| 配置方式 | 厂商 APP / nRF Connect | 厂商 APP / nRF Connect | Skylab_beacon APP（要联系销售给） |

**选 EW-BCN05L 的理由**：我们判定阈值是**在板子上现场标定**的，功率少 1.5 dB 完全能吸收；而 6 个月不用换电池是实打实的。CR2032 可更换。

**不选 VDB03S 的致命理由**：官方手册明写 ——「**广播频率的修改在按键按下和抬起、或者重新上电后会失效，需重新设置**」。换电池或按一下按键，广播间隔就退回 10s，判定直接失灵。发射功率也只有 0 dBm。

### 8.2 购买渠道

| 渠道 | 关键词 | 备注 |
|---|---|---|
| **淘宝（首选）** | `EW-BCN05L` | 店铺「**研为通讯科技(easywpy)官方企业店**」，约 **25~35 元**，个税运费后到付 |
| 淘宝 | `EW-BCN05` | 同店，+4dBm 强信号版，贵几元 |
| 1688 | `EW-BCN05` | 批量更便宜，但起订量高，个人不划算 |
| 京东 | `EW-BCN05L iBeacon 信标` | 第三方店铺，单件可买 |
| VDB03S | 淘宝「微能信息科技」/ 世强硬创 sekorm.com | 不推荐（见上） |

厂商官网：研为通讯 `www.easywphy.com`（淘宝店更便宜）；微能 `www.95power.com.cn`。

### 8.3 到了之后要做的三件事

1. **改 UUID / Major / Minor 成自己唯一的**
   默认 UUID 是 `01122334-4556-6778-899a-abbccddeeff0`（**所有出厂产品都一样**），Major/Minor 默认是随机滚码。家里要放两个就会撞。改成自己的，例如 UUID `E2C56DB5-DFFB-48D2-B060-D0F5A71096E0`、Major `1000`、Minor `1001`。
   → **必须用 EW SmartConfig（Android APP，向卖家要）来写**；nRF Connect 只能读不能写。
   → 改完用 `scripts/ble_rssi_probe.py --snapshot --ibeacon` 验证（会打印 UUID / major:minor / 实测广播间隔）

2. **广播间隔设 200ms**（理由见 8.4）
   ⚠️ EW SmartConfig 里 **min 和 max 都要设成 320**（单位 0.625ms）；**改小要先改 min 再改 max**，每次改完在数值小的那侧按 APPLY。

3. **绑定到设备**：`http://<设备IP>:8080/`
   - **MAC 模式**（推荐）：填 MAC → 出厂唯一，绝对不会撞，**且不用改 UUID**
   - **iBeacon 模式**：填 `E2C56DB5-DFFB-48D2-B060-D0F5A71096E0:1000:1001`（**major/minor 必须写全**，只填 UUID 会匹配上所有同 UUID 的信标）

   固件匹配规则（`arrival_target.cc`）：MAC 模式只比 6 字节地址；iBeacon 模式比 UUID 全等 + Major（填了才比）+ Minor（填了才比）。

### 8.4 ⚠️ 广播间隔直接决定判定延迟（重要耦合）

本模块的滤波窗口（`median_window` / `avg_window` / `hit_count`）**是按样本个数计的，不是按时间**。所以信标的广播间隔 = 判定延迟的尺度因子。

**Kconfig 默认值（median=5 / avg=10 / hit=3）正好是在 200ms 间隔的合成数据上标定并验证过的**（`test/ble_arrival/data/synthetic-walk.csv`，间隔 0.2s）：

| 广播间隔 | 中值窗口跨度 | 平均窗口跨度 | 到达延迟 | EW-BCN05L 续航 | 与现有验证 |
|---|---|---|---|---|---|
| 100 ms | 0.5 s | 1.0 s | ~0.8 s | ~3 个月 | 未验证（要重跑金标准） |
| **200 ms** | **1.0 s** | **2.0 s** | **+1.6 s（实测）** | **~6 个月** | ✅ **直接对应当前参数** |
| 500 ms | 2.5 s | 5.0 s | ~4 s | ~1.5 年 | 需改参数 |

> 换算规则：`样本数 = 想要的秒数 ÷ 广播间隔秒数`。若最终选了 500ms，Kconfig 里要把 `median_window` 降到 3、`avg_window` 降到 5、`hit_count` 降到 2，再重跑金标准。

**结论：选 200ms。** 既拿到了已验证过的 +1.6s 延迟和 6 个月续航，又不用改任何算法参数。

### 8.5 禁用清单（MAC 会随机化，绑不住）

- ❌ **AirTag / 防丢器** —— 广播是加密格式，UUID 和 MAC 都随机化
- ❌ **儿童手表** —— 加密 + 随机化
- ❌ **手机** —— iOS/Android 的 BLE 地址都做了随机化

✅ 华为手环/智能手表**通常**可以（Android 端广播 MAC 固定，实测可见且名字稳定），但广播间隔常常 1s 以上 → 判定延迟会到 ~8s。**可以用它零成本先跑通链路，正式用还是买信标。**

---

## 9. 与「豆包语音接入」的关系

两者互不依赖，可以同时跑：

- 本模块：**板子自己判断 + 自己播报**，不经过服务端
- 豆包语音：走服务端做 ASR/TTS/对话

本模块的播报走 `Application::Alert()`（屏幕文字 + 本地 assets 音效），**不需要服务端下发 notify**，所以自建服务端不支持 notify 也不影响它。

---

## 10. 对现有 AI 助手功能的影响评估

### 10.1 四件事逐项拆解

| 事 | 对 AI 助手功能的影响 | 说明 |
|---|---|---|
| **① 编译固件** | **零影响** | 编译发生在 PC 上，不碰设备。CI 跑挂了也不影响板子上现有的官方固件 |
| **② 烧录 + 配网 + 激活** | **唯一有实质影响的一步** | 见 10.2 / 10.3 |
| **③ 在板子上标定阈值** | **几乎零影响** | 只改 NVS 里的阈值数字和一次网页交互，不动固件逻辑 |
| **④ 买 BLE 信标** | **零影响** | 纯硬件，不与设备交互 |

### 10.2 「刷新固件」到底改变了什么

**不会变的**：

- 语音链路本身（唤醒词 / VAD / Opus / AEC / 豆包 ASR-TTS）代码路径**一行没动**
- 硬件外设：ES8311 / ES7210 / ST7789 / 摄像头 / 喇叭 / 按键，全部不变
- 云端配置、Token、设备绑定关系（烧整包固件会重置 NVS 里的设备身份 → **需要重新配网 + 重新激活**，但豆包/官方服务器侧的会话逻辑不变）
- `Alert()` 是**纯本地**动作：改屏幕文字 + 放本地 assets 音效，不发网络请求、不改设备状态机 → **不会打断正在进行的对话**

**会变的**：

| 变化 | 风险 | 缓解 |
|---|---|---|
| **内部 SRAM 少 100~140 KB** | ⚠️ **最大风险**。余量 171 KB，历史水位 72 KB | 见 §5.5 降级阶梯；刷完固件开机日志直接给出实测值 |
| **Wi-Fi 与 BLE 共用一颗 2.4GHz 射频** | ⚠️ 官方矩阵 C1（可用但性能不稳定）。可能表现为"说话时偶尔卡一下" | 50% 占空比 + 收音/说话/播报时**自动暂停扫描** + 软件共存默认开 |
| **多一个 BLE 控制器任务 + 我们的检测任务** | 抢占 CPU | 检测任务优先级 3，低于 audio_output(4) / opus(7) / audio_input(8) / 主任务(10) |
| **多一个 httpd :8080** | 多占约 6~8 KB RAM + 一个端口 | `CONFIG_BLE_ARRIVAL_WEB_ENABLED=n` 可关；仅绑定期需要 |
| **变体名变成 `lichuang-dev-ble`** | 官方 OTA 通道里没有这个变体 → **以后升级只能自己编、自己刷** | 这也是保护：官方 OTA 不会把 BLE 功能覆盖掉 |
| **屏幕气泡被覆盖** | 到达提示会把聊天气泡换成"宝贝到门口啦"，直到下一条消息 | 提示本身是有意为之；可改文案 |

### 10.3 结论

- **功能正确性：不受影响。** 语音助手的代码路径完全没碰，播报是纯本地动作，不改状态机、不抢状态。
- **性能：主要看内部 SRAM 和射频共存这两项。** 代码层已经把能做的都做了（优先级、占空比、自动暂停）；剩下的**必须上板实测**，好在固件开机日志自带对照数字（官方基线 `free=175239 / minimal=73712`）。
- **可逆**：随时可以刷回 `firmware/extracted/merged-binary.bin`（官方 `lckfb-lichuang-dev` 整包）恢复原状，assets 分区不动。

### 10.4 刷完固件后按这个顺序验收

1. **看开机日志** → 确认 `蓝牙栈初始化后` 的 `free sram` 明显大于 0，且 `minimal sram` 没有跌到个位数 KB
2. **说三句话** → 唤醒、回答、播报都正常（射频共存实测）
3. **收音/说话时看 `scan_pauses_`** → 蓝牙状态页上"暂停次数"应该在说话时增长，说明自动让频生效了
4. **站门口/退到楼下** → 按 §4 标定阈值
