#!/usr/bin/env python3
"""生成英语学习音频的编译期索引头文件。

背景
----
固件用 ESP-IDF 的 ``EMBED_FILES`` 把 ogg 音频编进 app 分区。IDF 会为每个被嵌入的
文件生成一对链接符号::

    _binary_<路径>_start / _binary_<路径>_end

问题在于 ``<路径>`` 的取法在不同 IDF 版本/调用方式下并不一致：

* 只取文件名        -> ``_binary_en_u3_w05_ogg_start``
* 相对组件目录      -> ``_binary_assets_english_en_u3_w05_ogg_start``
* 相对工程根目录    -> ``_binary_main_assets_english_en_u3_w05_ogg_start``

上游 xiaozhi-esp32 用的是第一种（见 ``scripts/gen_lang.py``），但我们无法在本机
编译验证，一旦猜错就是链接期 undefined reference。

对策
----
三种取法**全部声明成弱符号**（``__attribute__((weak))``）。弱符号未定义时链接器
不会报错，运行时取值为 0；``english_audio.cc`` 在启动时挑第一个能解析出来的候选。
这样无论构建机上的 IDF 用哪种规则都能工作，且不会因为符号缺失导致构建失败。

用法::

    python scripts/gen_english_audio_index.py \\
        --assets main/assets/english \\
        --output main/english_learning/english_audio_table.h
"""

from __future__ import annotations

import argparse
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent
DEFAULT_ASSETS = REPO_ROOT / "main" / "assets" / "english"
DEFAULT_OUTPUT = REPO_ROOT / "main" / "english_learning" / "english_audio_table.h"

# 三种候选的符号前缀，按优先级排列
CANDIDATE_PREFIXES = (
    "_binary_{stem}_ogg",
    "_binary_assets_english_{stem}_ogg",
    "_binary_main_assets_english_{stem}_ogg",
)

HEADER_TEMPLATE = """// 由 english-learning/scripts/gen_audio_index.py 自动生成，请勿手改。
// 源目录: {assets_dir}
//
// 每个音频声明了 {n_candidates} 个候选链接符号（弱引用）。ESP-IDF 的 EMBED_FILES
// 对嵌入文件生成的符号名取决于路径取法，不同版本有差异，这里全部声明出来，
// 运行时挑第一个能解析的。详见 english_audio.cc。
#pragma once

#include <stddef.h>

namespace english_learning {{

/** 一段编进固件的 ogg 音频。 */
struct EnglishAudioAsset {{
    const char* name;      /**< 资源名，如 "en_u3_w05.ogg" */
    const char* start[{n_candidates}];  /**< 候选起始符号，未定义时为 nullptr */
    const char* end[{n_candidates}];    /**< 候选结束符号，未定义时为 nullptr */
}};

extern "C" {{
{externs}
}}

static const EnglishAudioAsset kEnglishAudioAssets[] = {{
{table}
}};

static const size_t kEnglishAudioCount = sizeof(kEnglishAudioAssets) / sizeof(kEnglishAudioAssets[0]);

}}  // namespace english_learning
"""


def build_extern(stem: str) -> str:
    """为一个文件生成三个候选符号的弱引用声明。

    C++ 侧的标识符必须每个候选都不同（否则是重复声明），链接器实际看的
    是 __asm__ 里给的名字。
    """
    lines = []
    for index, prefix in enumerate(CANDIDATE_PREFIXES):
        symbol = prefix.format(stem=stem)
        alias = f"{stem}_ogg_c{index}"
        lines.append(
            f'    extern const char {alias}_start[] __asm__("{symbol}_start") '
            f'__attribute__((weak));'
        )
        lines.append(
            f'    extern const char {alias}_end[] __asm__("{symbol}_end") '
            f'__attribute__((weak));'
        )
    return "\n".join(lines)


def build_entry(name: str, stem: str) -> str:
    starts = ", ".join(f"{stem}_ogg_c{i}_start" for i in range(len(CANDIDATE_PREFIXES)))
    ends = ", ".join(f"{stem}_ogg_c{i}_end" for i in range(len(CANDIDATE_PREFIXES)))
    return f'    {{"{name}",\n     {{{starts}}},\n     {{{ends}}}}},'


def main() -> int:
    parser = argparse.ArgumentParser(description="生成英语学习音频的编译期索引头文件")
    parser.add_argument("--assets", default=str(DEFAULT_ASSETS), help="ogg 音频目录")
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT), help="输出的头文件路径")
    args = parser.parse_args()

    assets_dir = Path(args.assets)
    output = Path(args.output)

    files = sorted(p for p in assets_dir.glob("*.ogg") if p.is_file())
    if not files:
        raise SystemExit(f"没有找到任何 ogg：{assets_dir}")

    externs = []
    entries = []
    total_bytes = 0
    for path in files:
        stem = path.stem
        externs.append(build_extern(stem))
        entries.append(build_entry(path.name, stem))
        total_bytes += path.stat().st_size

    content = HEADER_TEMPLATE.format(
        assets_dir=assets_dir.as_posix(),
        n_candidates=len(CANDIDATE_PREFIXES),
        externs="\n".join(externs),
        table="\n".join(entries),
    )

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(content, encoding="utf-8")

    print(f"音频索引已生成: {output}")
    print(f"  音频文件: {len(files)} 个")
    print(f"  总体积:   {total_bytes / 1024:.1f} KB")
    print(f"  候选符号: {len(CANDIDATE_PREFIXES)} 种/文件")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
