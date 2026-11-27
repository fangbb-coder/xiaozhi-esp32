#!/usr/bin/env python3
"""
arrival_algo.py —— arrival_detector.c 的 Python 参照实现 + 回放/金标准生成工具

用途：
  1. 与 C 实现逐条对齐（scripts/ 里由 tools/gen_golden.py 生成的 C 断言表，
     在主机侧用 zig cc/gcc 编译运行，验证 C 与 Python 语义完全一致）。
  2. 用真实采集到的 BLE RSSI 数据回放，标定阈值 / 滤波参数。

严格对齐的语义点（改 C 实现时必须同步改这里）：
  * 中值窗口样本数为偶数时取中间两个的算术平均，整数除法**向零截断**。
  * 滑动平均同样是向零截断。
  * 无符号时间差（天然兼容 32 位回绕）。

命令行：
  python arrival_algo.py replay  <csv> [--mac AA:BB:..] [--label 标签]
                                 [--enter -70] [--hysteresis 12]
                                 [--median 5] [--avg 10] [--hit 3] [--miss 4]
                                 [--cooldown 60000] [--stale 5000] [--tick 500]
  python arrival_algo.py golden <csv> --out golden.h
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import sys
from dataclasses import dataclass, field

ARRIVAL_MAX_MEDIAN_WINDOW = 15
ARRIVAL_MAX_AVG_WINDOW = 30
ARRIVAL_RSSI_INVALID = 127

EVENT_NONE = 0
EVENT_ARRIVED = 1
EVENT_LEFT = -1

STATE_UNKNOWN = 0
STATE_ABSENT = 1
STATE_PRESENT = 2

EVENT_NAMES = {EVENT_NONE: "NONE", EVENT_ARRIVED: "ARRIVED", EVENT_LEFT: "LEFT"}
STATE_NAMES = {STATE_UNKNOWN: "UNKNOWN", STATE_ABSENT: "ABSENT", STATE_PRESENT: "PRESENT"}


@dataclass
class ArrivalConfig:
    median_window: int = 5
    avg_window: int = 10
    enter_threshold: int = -70
    hysteresis_db: int = 12
    hit_count: int = 3
    miss_count: int = 4
    cooldown_ms: int = 60000
    stale_timeout_ms: int = 8000

    def sanitized(self) -> "ArrivalConfig":
        c = ArrivalConfig(**self.__dict__)
        c.median_window = max(1, min(ARRIVAL_MAX_MEDIAN_WINDOW, c.median_window))
        c.avg_window = max(1, min(ARRIVAL_MAX_AVG_WINDOW, c.avg_window))
        if c.enter_threshold > 0:
            c.enter_threshold = -1
        c.hysteresis_db = min(127, c.hysteresis_db)
        c.hit_count = max(1, c.hit_count)
        c.miss_count = max(1, c.miss_count)
        return c

    @property
    def leave_threshold(self) -> int:
        return self.enter_threshold - self.hysteresis_db


@dataclass
class ArrivalDetector:
    cfg: ArrivalConfig = field(default_factory=ArrivalConfig)

    def __post_init__(self):
        self.cfg = self.cfg.sanitized()
        self.median_buf: list[int] = []
        self.avg_buf: list[int] = []
        self.avg_sum = 0
        self.state = STATE_UNKNOWN
        self.hit_streak = 0
        self.miss_streak = 0
        self.last_rx_ms = 0
        self.last_arrived_ms = 0
        self.arrived_count = 0
        self.left_count = 0
        self.sample_count = 0
        self.last_raw_rssi = 0
        self.last_filtered_rssi = ARRIVAL_RSSI_INVALID

    # ---- 内部工具 ----
    def _median(self) -> int:
        buf = self.median_buf
        count = len(buf)
        if count == 0:
            return ARRIVAL_RSSI_INVALID
        s = sorted(buf)
        if count & 1:
            return s[count // 2]
        # 偶数：中间两个的算术平均，向零截断（与 C 一致）
        return int((s[count // 2 - 1] + s[count // 2]) / 2)

    def filtered_rssi(self) -> int:
        if not self.avg_buf:
            return ARRIVAL_RSSI_INVALID
        return int(self.avg_sum / len(self.avg_buf))

    # ---- 对外接口 ----
    def reset(self) -> None:
        self.median_buf = []
        self.avg_buf = []
        self.avg_sum = 0
        self.hit_streak = 0
        self.miss_streak = 0
        self.last_filtered_rssi = ARRIVAL_RSSI_INVALID

    def _check_stale(self, now_ms: int) -> int:
        if (
            self.state == STATE_PRESENT
            and self.cfg.stale_timeout_ms > 0
            and (now_ms - self.last_rx_ms) > self.cfg.stale_timeout_ms
        ):
            self.state = STATE_ABSENT
            self.hit_streak = 0
            self.miss_streak = 0
            self.left_count += 1
            return EVENT_LEFT
        return EVENT_NONE

    def feed(self, rssi: int, now_ms: int) -> int:
        if rssi == ARRIVAL_RSSI_INVALID or rssi > 0:
            return self._check_stale(now_ms)

        self.last_raw_rssi = rssi
        self.last_rx_ms = now_ms
        self.sample_count += 1

        # 1) 中值滤波
        self.median_buf.append(rssi)
        if len(self.median_buf) > self.cfg.median_window:
            self.median_buf.pop(0)
        med = self._median()

        # 2) 滑动平均
        self.avg_buf.append(med)
        if len(self.avg_buf) > self.cfg.avg_window:
            self.avg_sum -= self.avg_buf.pop(0)
        self.avg_sum += med

        filtered = self.filtered_rssi()
        self.last_filtered_rssi = filtered

        enter = self.cfg.enter_threshold
        leave = self.cfg.leave_threshold

        if filtered >= enter:
            self.miss_streak = 0
            self.hit_streak = min(255, self.hit_streak + 1)
            if self.state != STATE_PRESENT:
                if self.hit_streak >= self.cfg.hit_count:
                    self.state = STATE_PRESENT
                    self.hit_streak = 0
                    cooled = self.arrived_count == 0 or (
                        now_ms - self.last_arrived_ms
                    ) >= self.cfg.cooldown_ms
                    if cooled:
                        self.last_arrived_ms = now_ms
                        self.arrived_count += 1
                        return EVENT_ARRIVED
            else:
                self.hit_streak = 0
        elif filtered < leave:
            self.hit_streak = 0
            self.miss_streak = min(255, self.miss_streak + 1)
            if self.state == STATE_PRESENT and self.miss_streak >= self.cfg.miss_count:
                self.state = STATE_ABSENT
                self.miss_streak = 0
                self.left_count += 1
                return EVENT_LEFT
            if self.state == STATE_UNKNOWN:
                self.state = STATE_ABSENT
        else:
            self.hit_streak = 0
            self.miss_streak = 0

        return EVENT_NONE


# --------------------------------------------------------------------------
# CSV 回放
# --------------------------------------------------------------------------
def load_samples(csv_path: str, mac: str | None, label: str | None):
    """读取采集 CSV，按时间排序，返回 [(ts_ms, rssi), ...]"""
    rows = []
    mac_filter = mac.upper() if mac else None
    with open(csv_path, "r", encoding="utf-8", newline="") as fh:
        for rec in csv.DictReader(fh):
            if mac_filter and (rec.get("mac") or "").upper() != mac_filter:
                continue
            if label and (rec.get("label") or "") != label:
                continue
            try:
                ts = float(rec["ts"])
                rssi = int(rec["rssi"])
            except (KeyError, TypeError, ValueError):
                continue
            rows.append((ts, rssi))
    rows.sort(key=lambda x: x[0])
    return rows


def replay(samples, cfg: ArrivalConfig, tick_ms: int):
    """
    按真实时间轴回放：包到达时喂样本，没包时按 tick_ms 周期喂非法 RSSI 驱动超时检查。
    返回事件列表 [(event, ts_ms, filtered_rssi), ...]
    """
    det = ArrivalDetector(cfg)
    events = []
    if not samples:
        return events, det

    t0_ms = int(samples[0][0] * 1000)
    next_tick = t0_ms
    for ts, rssi in samples:
        ts_ms = int(ts * 1000)
        # 先把 tick 补到当前包之前
        while next_tick <= ts_ms:
            det.feed(ARRIVAL_RSSI_INVALID, next_tick)
            next_tick += tick_ms
        ev = det.feed(rssi, ts_ms)
        if ev != EVENT_NONE:
            events.append((ev, ts_ms, det.last_filtered_rssi))
    # 收尾：再补几个 tick，让 stale 超时有机会触发
    for _ in range(20):
        det.feed(ARRIVAL_RSSI_INVALID, next_tick)
        next_tick += tick_ms
    return events, det


def load_labels(csv_path: str):
    """读取 CSV 的标签分段 [(起始 ts, 标签), ...]，用于和检测事件对齐看延迟"""
    segments = []
    with open(csv_path, "r", encoding="utf-8", newline="") as fh:
        for rec in csv.DictReader(fh):
            label = rec.get("label") or ""
            if not label:
                continue
            try:
                ts = float(rec["ts"])
            except (KeyError, TypeError, ValueError):
                continue
            if not segments or segments[-1][1] != label:
                segments.append((ts, label))
    return segments


def cmd_replay(args) -> int:
    cfg = ArrivalConfig(
        median_window=args.median,
        avg_window=args.avg,
        enter_threshold=args.enter,
        hysteresis_db=args.hysteresis,
        hit_count=args.hit,
        miss_count=args.miss,
        cooldown_ms=args.cooldown,
        stale_timeout_ms=args.stale,
    )
    samples = load_samples(args.csv, args.mac, args.label)
    if not samples:
        print("没有匹配的样本（检查 --mac / --label）", file=sys.stderr)
        return 2
    events, det = replay(samples, cfg, args.tick)

    t0 = int(samples[0][0] * 1000)
    print(f"样本数: {len(samples)}   事件数: {len(events)}")
    print(
        f"参数: median={cfg.median_window} avg={cfg.avg_window} "
        f"enter={cfg.enter_threshold} leave={cfg.leave_threshold} "
        f"hit={cfg.hit_count} miss={cfg.miss_count} "
        f"cooldown={cfg.cooldown_ms}ms stale={cfg.stale_timeout_ms}ms"
    )
    if events:
        print("\n事件时间线（相对数据起点）:")
        for ev, ts_ms, filt in events:
            print(f"  {(ts_ms - t0) / 1000:8.2f}s  {EVENT_NAMES[ev]:<8} filtered={filt}dBm")
    else:
        print("\n没有产生任何事件（阈值不合适或数据里没有靠近/离开过程）")
    print(
        f"\n最终状态: {STATE_NAMES[det.state]}  "
        f"arrived={det.arrived_count} left={det.left_count} "
        f"最后滤波值={det.last_filtered_rssi}dBm"
    )

    segments = load_labels(args.csv)
    if len(segments) > 1:
        base_ts = segments[0][0]
        print("\n真值分段（相对数据起点）:")
        for i, (ts, label) in enumerate(segments):
            end = segments[i + 1][0] if i + 1 < len(segments) else None
            span = f"{(end - ts):6.1f}s" if end is not None else "   结束"
            print(f"  {(ts - base_ts):8.2f}s  {label:<10} 持续 {span}")
    return 0


def cmd_golden(args) -> int:
    """
    生成 C 侧断言用的金标准表：
      - 用 Python 参照实现跑一遍真实数据；
      - 输出 (rssi, dt_ms, 期望事件, 期望滤波值) 序列到 C 头文件。
    C 测试按同样顺序喂入，逐条比对。
    """
    cfg = ArrivalConfig(
        median_window=args.median,
        avg_window=args.avg,
        enter_threshold=args.enter,
        hysteresis_db=args.hysteresis,
        hit_count=args.hit,
        miss_count=args.miss,
        cooldown_ms=args.cooldown,
        stale_timeout_ms=args.stale,
    )
    samples = load_samples(args.csv, args.mac, args.label)
    if not samples:
        print("没有匹配的样本", file=sys.stderr)
        return 2

    # 构造与固件一致的输入序列：包到达喂 RSSI，空档按 tick 喂非法值
    tick_ms = args.tick
    det = ArrivalDetector(cfg)
    rows = []
    t0_ms = int(samples[0][0] * 1000)
    next_tick = t0_ms
    for ts, rssi in samples:
        ts_ms = int(ts * 1000)
        while next_tick <= ts_ms:
            ev = det.feed(ARRIVAL_RSSI_INVALID, next_tick)
            rows.append((ARRIVAL_RSSI_INVALID, next_tick - t0_ms, ev, det.last_filtered_rssi))
            next_tick += tick_ms
        ev = det.feed(rssi, ts_ms)
        rows.append((rssi, ts_ms - t0_ms, ev, det.last_filtered_rssi))

    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("/* 自动生成，请勿手工编辑。\n")
        fh.write(f" * 来源: {os.path.basename(args.csv)}\n")
        fh.write(f" * 参数: median={cfg.median_window} avg={cfg.avg_window} "
                 f"enter={cfg.enter_threshold} hysteresis={cfg.hysteresis_db} "
                 f"hit={cfg.hit_count} miss={cfg.miss_count} "
                 f"cooldown={cfg.cooldown_ms} stale={cfg.stale_timeout_ms} tick={tick_ms}\n")
        fh.write(" * 生成命令见 test/ble_arrival/README.md\n */\n")
        fh.write("#ifndef ARRIVAL_GOLDEN_VECTORS_H_\n#define ARRIVAL_GOLDEN_VECTORS_H_\n\n")
        fh.write("#include <stdint.h>\n\n")
        fh.write("/* 生成时使用的参数，C 侧必须用同样的参数重放 */\n")
        fh.write(f"#define ARRIVAL_GOLDEN_MEDIAN_WINDOW {cfg.median_window}\n")
        fh.write(f"#define ARRIVAL_GOLDEN_AVG_WINDOW {cfg.avg_window}\n")
        fh.write(f"#define ARRIVAL_GOLDEN_ENTER_THRESHOLD ({cfg.enter_threshold})\n")
        fh.write(f"#define ARRIVAL_GOLDEN_HYSTERESIS_DB {cfg.hysteresis_db}\n")
        fh.write(f"#define ARRIVAL_GOLDEN_HIT_COUNT {cfg.hit_count}\n")
        fh.write(f"#define ARRIVAL_GOLDEN_MISS_COUNT {cfg.miss_count}\n")
        fh.write(f"#define ARRIVAL_GOLDEN_COOLDOWN_MS {cfg.cooldown_ms}\n")
        fh.write(f"#define ARRIVAL_GOLDEN_STALE_TIMEOUT_MS {cfg.stale_timeout_ms}\n")
        fh.write(f"#define ARRIVAL_GOLDEN_TICK_MS {tick_ms}\n\n")
        fh.write("typedef struct {\n")
        fh.write("    int8_t rssi;\n")
        fh.write("    uint32_t dt_ms;\n")
        fh.write("    int8_t expected_event;\n")
        fh.write("    int8_t expected_filtered;\n")
        fh.write("} arrival_golden_step_t;\n\n")
        fh.write(f"#define ARRIVAL_GOLDEN_STEPS {len(rows)}\n")
        fh.write("static const arrival_golden_step_t arrival_golden_steps[ARRIVAL_GOLDEN_STEPS] = {\n")
        for rssi, dt, ev, filt in rows:
            fh.write(f"    {{ {rssi}, {dt}u, {ev}, {filt} }},\n")
        fh.write("};\n\n#endif  // ARRIVAL_GOLDEN_VECTORS_H_\n")

    print(f"已生成 {args.out}（{len(rows)} 步）")
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="BLE 到达判定参照实现 / 回放工具")
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_common(sp):
        sp.add_argument("csv", help="采集 CSV（ts,label,mac,name,rssi,...）")
        sp.add_argument("--mac", help="只取该 MAC")
        sp.add_argument("--label", help="只取该标签")
        sp.add_argument("--median", type=int, default=5)
        sp.add_argument("--avg", type=int, default=10)
        sp.add_argument("--enter", type=int, default=-70)
        sp.add_argument("--hysteresis", type=int, default=12)
        sp.add_argument("--hit", type=int, default=3)
        sp.add_argument("--miss", type=int, default=4)
        sp.add_argument("--cooldown", type=int, default=60000)
        sp.add_argument("--stale", type=int, default=8000)
        sp.add_argument("--tick", type=int, default=500, help="空档喂非法样本的周期(ms)")

    sp = sub.add_parser("replay", help="回放真实采集数据")
    add_common(sp)
    sp.set_defaults(func=cmd_replay)

    sp = sub.add_parser("golden", help="生成 C 金标准向量")
    add_common(sp)
    sp.add_argument("--out", required=True)
    sp.set_defaults(func=cmd_golden)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
