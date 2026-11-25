#!/usr/bin/env python3
"""
make_synthetic_walk.py —— 生成"走近 -> 停留 -> 离开"的标定数据集

为什么要合成数据：
  真实采集（2026-11-23 办公环境）只有**单一距离**（办公位），无法覆盖
  "到达 / 离开"的状态跃迁；而金标准向量必须能同时覆盖 ARRIVED 和 LEFT 两条路径。

  本脚本用**实测噪声特征**合成：
    - 实测固定设备 RSSI 标准差 0.88 ~ 4.8 dB
    - 实测移动设备（华为手表）标准差 6.3 dB
    - 实测广播到达间隔中位数 117 ~ 950 ms，最大空档 4.7 s
  距离档位取自 RSSI 经验值（近/中/远），**不是**实测的门口数据 ——
  真正的阈值仍必须在设备自身上标定。

输出: test/ble_arrival/data/synthetic-walk.csv（格式与 ble_rssi_probe.py 一致）
"""

import csv
import os
import random

# 距离档位: (标签, 基础 RSSI dBm, 噪声标准差 dB)
SEGMENTS = [
    ("远场", 20, -85, 3.0),
    ("走向门口", 10, -76, 4.5),
    ("门口停留", 30, -61, 5.0),
    ("离开中", 12, -75, 4.5),
    ("已远离", 20, -86, 3.0),
]

SAMPLE_INTERVAL_MS = 200  # 模拟 50% 扫描占空比 + 信标 100ms 广播间隔


def main() -> None:
    random.seed(20261123)  # 固定种子，保证金标准向量可复现

    here = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.join(os.path.dirname(here), "data", "synthetic-walk.csv")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    mac = "AA:BB:CC:DD:EE:01"
    ts = 1795400000.0
    rows = []
    sample_index = 0

    for label, seconds, base, sigma in SEGMENTS:
        n = int(seconds * 1000 / SAMPLE_INTERVAL_MS)
        for _ in range(n):
            rssi = int(round(random.gauss(base, sigma)))
            rssi = max(-95, min(-30, rssi))
            # 每 37 个样本掺一个毛刺，检验中值滤波
            if sample_index % 37 == 11:
                rssi = base - 25
            rows.append((ts, label, mac, "", rssi, "", ""))
            ts += SAMPLE_INTERVAL_MS / 1000.0
            sample_index += 1

    with open(out_path, "w", encoding="utf-8", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["ts", "label", "mac", "name", "rssi", "tx_power", "mfg_ids"])
        writer.writerows(rows)

    print(f"已生成 {out_path}")
    print(f"样本数 {len(rows)}，跨度 {rows[-1][0] - rows[0][0]:.1f}s")
    for label, seconds, base, sigma in SEGMENTS:
        sub = [r[4] for r in rows if r[1] == label]
        print(f"  {label:<10} n={len(sub):<4} 目标 {base}dBm  实测均值 {sum(sub)/len(sub):.1f}dBm")


if __name__ == "__main__":
    main()
