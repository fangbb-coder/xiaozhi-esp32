#!/usr/bin/env python3
"""把单元词汇表 JSON 转成编译期 C++ 头文件（零 RAM 占用）。

用法
----
    python scripts/gen_english_word_table.py
    python scripts/gen_english_word_table.py --data main/english_learning/pep4a_words.json \\
        --out main/english_learning/word_table.h

产物说明
--------
* 词表、单元标题全是 `constexpr` 字符串字面量 → 落在 flash，不占内部 SRAM
* 每条词带三个音频资源名（en / spell / zh），命名规则与 gen_audio.py 一致：
      en_u{unit}_w{idx}.ogg        单词发音
      en_u{unit}_w{idx}_spell.ogg  逐字母拼读
      zh_u{unit}_w{idx}.ogg        汉语意思
* `spell` 是给屏幕看的「字母拆解」串：小写 + 连字符，词与词之间用空格
  例：playground -> p-l-a-y-g-r-o-u-n-d
      office worker -> o-f-f-i-c-e w-o-r-k-e-r
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent
DEFAULT_DATA = REPO_ROOT / "main" / "english_learning" / "pep4a_words.json"
DEFAULT_OUT = REPO_ROOT / "main" / "english_learning" / "word_table.h"

# 保留字符：字母、数字、连字符、撇号；其余一律跳过
_KEEP = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-'")


def spell_out(word: str) -> str:
    """把单词拆成逐字母形式：playground -> p-l-a-y-g-r-o-u-n-d（全小写）"""
    parts = []
    for token in word.split():
        letters = [c for c in token if c in _KEEP]
        if letters:
            parts.append("-".join(ch.lower() for ch in letters))
    return " ".join(parts)


def c_str(value: str) -> str:
    """转成 C++ 字符串字面量（含 UTF-8 原样保留）。"""
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def main() -> int:
    ap = argparse.ArgumentParser(description="生成英语单词表的编译期 C++ 头文件")
    ap.add_argument("--data", default=str(DEFAULT_DATA))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    args = ap.parse_args()

    data_path = Path(args.data)
    if not data_path.exists():
        print(f"[FAIL] 找不到词表数据: {data_path}")
        return 1
    payload = json.loads(data_path.read_text(encoding="utf-8"))
    units = payload.get("units") or []
    if not units:
        print(f"[FAIL] 词表里没有 units: {data_path}")
        return 1

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    lines: list[str] = []
    add = lines.append
    add("// 由 scripts/gen_english_word_table.py 自动生成，请勿手改。")
    add("// 源数据: main/english_learning/pep4a_words.json")
    add("#pragma once")
    add("")
    add("#include <stdint.h>")
    add("")
    add("namespace english_learning {")
    add("")
    add("/** 一条词条。所有字符串都是 flash 里的字面量，不占 RAM。 */")
    add("struct WordEntry {")
    add("    const char* en;      // 单词，如 \"playground\"")
    add("    const char* zh;      // 汉语意思，如 \"游乐场；操场\"")
    add("    const char* spell;   // 逐字母拆解，屏幕上显示，如 \"P-L-A-Y-G-R-O-U-N-D\"")
    add("    uint16_t en_len;")
    add("    uint16_t zh_len;")
    add("    uint16_t spell_len;")
    add("};")
    add("")
    add("/** 一个单元的词表。 */")
    add("struct UnitEntry {")
    add("    const char* title;      // 单元标题，如 \"Unit 3\"")
    add("    const WordEntry* words;")
    add("    uint16_t word_count;")
    add("};")
    add("")

    total_words = 0
    for unit in units:
        u = int(unit["unit"])
        title = unit.get("title") or f"Unit {u}"
        words = unit.get("words") or []
        if not words:
            print(f"[WARN] Unit {u} 没有词条")
            continue
        total_words += len(words)

        add(f"// ---- Unit {u}: {title} ({len(words)} 词) ----")
        for idx, w in enumerate(words):
            en = w["en"].strip()
            zh = w.get("zh", "").strip()
            spell = spell_out(en)
            if not spell:
                print(f"[WARN] Unit {u} 第 {idx + 1} 个词 '{en}' 拆不出字母，已跳过")
                continue
            add(
                f"static const WordEntry kU{u}W{idx + 1} = {{"
                f"{c_str(en)}, {c_str(zh)}, {c_str(spell)}, "
                f"{len(en.encode('utf-8'))}, {len(zh.encode('utf-8'))}, {len(spell.encode('utf-8'))}}};"
            )
        add("")
        add(f"static const WordEntry kUnit{u}Words[] = {{")
        for idx in range(len(words)):
            add(f"    kU{u}W{idx + 1},")
        add("};")
        add("")

    add("static const UnitEntry kEnglishUnits[] = {")
    for unit in units:
        u = int(unit["unit"])
        title = unit.get("title") or f"Unit {u}"
        add(f"    {{{c_str(title)}, kUnit{u}Words, {len(unit.get('words') or [])}}},")
    add("};")
    add("")
    add("static const uint16_t kEnglishUnitCount = "
        f"{len(units)};")
    add("")
    add("}  // namespace english_learning")
    add("")

    out_path.write_text("\n".join(lines), encoding="utf-8")

    # ---- 打印摘要 + 校验 ----
    print(f"[OK] 生成: {out_path}")
    print(f"     单元 {len(units)} 个，词条 {total_words} 条，"
          f"文件 {out_path.stat().st_size / 1024:.1f} KB")
    longest = max((len(w["en"]) for u in units for w in u["words"]), default=0)
    longest_spell = max(
        (len(spell_out(w["en"])) for u in units for w in u["words"]), default=0
    )
    print(f"     最长单词 {longest} 字符，最长拼写串 {longest_spell} 字符")
    if longest_spell > 26:
        print(f"[WARN] 最长拼写串 {longest_spell} 字符，320px 屏在 20px 字号下可能换行")
    return 0


if __name__ == "__main__":
    sys.exit(main())
