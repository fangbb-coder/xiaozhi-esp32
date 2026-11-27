# 到达判定核心的测试与金标准

本目录是 `main/ble_arrival/arrival_detector.{h,c}`（纯 C99 核心）的**主机侧**测试。

核心算法刻意写成不依赖 ESP-IDF、不依赖 STL、没有动态内存的纯 C，因此可以在任意一台装了 gcc/clang 的机器上编译运行 —— 包括没有 ESP-IDF 工具链的 Windows。

---

## 1. 文件

| 文件 | 说明 |
|---|---|
| `arrival_detector_test.c` | 单元测试：基础算子 / 事件时序 / 超时判定 / 金标准交叉校验 |
| `arrival_algo.py` | **Python 参照实现** + 回放工具 + 金标准生成器（与 C 实现语义严格一致） |
| `arrival_golden_vectors.h` | 自动生成的金标准向量（C 侧逐条比对用） |
| `tools/make_synthetic_walk.py` | 生成「走近 → 停留 → 离开」标定数据集 |
| `data/synthetic-walk.csv` | 合成走位数据（噪声特征取自 2026-11-23 实测） |
| `data/measured-scan-watch.csv` | **真实**采集数据（办公环境，单一距离，640 条） |
| `data/measured-scan-env.csv` | **真实**采集数据（环境基线，1046 条 / 78 个设备） |

---

## 2. 跑测试

```bash
gcc -std=c99 -Wall -Wextra -Werror -O2 \
    -I main/ble_arrival -I test/ble_arrival \
    test/ble_arrival/arrival_detector_test.c \
    main/ble_arrival/arrival_detector.c \
    -o /tmp/arrival_test
/tmp/arrival_test
```

期望输出：

```
== arrival_detector 单元测试 ==
- 基础算子
- 事件时序
- 超时判定
- 金标准交叉校验
  金标准比对: 644 步全部一致

断言 65 条，失败 0 条
```

Windows（用项目里自带的便携 GCC）：

```powershell
$gcc = 'D:\程序\08-utility-tools\xiaozhi-esp32-doubao\tools\w64devkit\w64devkit\bin\gcc.exe'
& $gcc -std=c99 -Wall -Wextra -Werror -O2 `
  -I '..\..\main\ble_arrival' -I . `
  arrival_detector_test.c '..\..\main\ble_arrival\arrival_detector.c' -o arrival_test.exe
.\arrival_test.exe
```

---

## 3. 回放真实数据

```bash
# 指定 MAC 回放
python3 test/ble_arrival/arrival_algo.py replay \
    test/ble_arrival/data/measured-scan-watch.csv --mac B0:FE:E5:F5:C5:2E

# 合成走位数据回放（带真值分段对齐）
python3 test/ble_arrival/arrival_algo.py replay \
    test/ble_arrival/data/synthetic-walk.csv
```

输出会给出：事件时间线（相对数据起点）、真值分段、最终状态。

---

## 4. 重新生成金标准向量

改了 `arrival_detector.c` 或 `arrival_algo.py` 的语义之后，**必须**重新生成并让 C 测试重新通过：

```bash
python3 test/ble_arrival/arrival_algo.py golden \
    test/ble_arrival/data/synthetic-walk.csv \
    --out test/ble_arrival/arrival_golden_vectors.h
```

生成过程是确定性的（固定随机种子 + 固定 CSV），所以任何机器上重新生成都应得到**逐字节相同**的文件。GitHub Actions 的 `ble-arrival.yml` 会校验这一点：

```
python3 test/ble_arrival/arrival_algo.py golden ... --out /tmp/golden.h
diff -u test/ble_arrival/arrival_golden_vectors.h /tmp/golden.h
```

> 这条校验的意义：**C 实现和 Python 参照实现必须永远等价**。
> 只要有一边被改了而另一边没跟上，diff 立刻失败。

---

## 5. 重新生成合成数据集

```bash
python3 test/ble_arrival/tools/make_synthetic_walk.py
```

固定种子 `20261123`，输出到 `data/synthetic-walk.csv`。

合成数据的参数来自 2026-11-23 的实测：

- 固定设备 RSSI 标准差 0.88 ~ 4.8 dB
- 移动设备（华为手表）标准差 6.3 dB
- 广播到达间隔中位数 117 ~ 950 ms，最大空档 4.7 s

距离档位（远/中/近）是 RSSI 经验值，**不是**实测的门口数据 —— 真正的阈值必须在设备自身上标定。

---

## 6. C 与 Python 的语义对齐点

改任何一边都要同步另一边。对齐规则：

1. 中值窗口样本数为**偶数**时取中间两个的算术平均，整数除法**向零截断**
2. 滑动平均同样向零截断
3. 时间差用无符号相减（兼容 uint32 回绕）
4. 状态机分支顺序、冷却期判定、stale 超时判定完全一致

Python 侧用 `math.trunc` / `int()` 对齐 C 的向零截断（**注意**：Python 的 `//` 是向下取整，负数上会和 C 不一致）。
