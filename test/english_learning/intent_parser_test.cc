/**
 * @file intent_parser_test.cc
 * @brief intent_parser 的主机侧单元测试
 *
 * 编译（不依赖 ESP-IDF）：
 *   g++ -std=c++17 -Wall -Wextra -Werror -O2 \
 *       -I ../../main/english_learning \
 *       intent_parser_test.cc ../../main/english_learning/intent_parser.cc -o intent_test
 */
#include <cstdio>
#include <string>

#include "intent_parser.h"

namespace {

int g_total = 0;
int g_failed = 0;

void CheckUnit(const std::string& text, int want_unit) {
    g_total++;
    english_learning::ParsedIntent p = english_learning::ParseIntent(text, 6);
    bool ok = (p.intent == english_learning::Intent::kSelectUnit && p.unit == want_unit);
    if (!ok) {
        g_failed++;
        std::printf("  [FAIL] \"%s\" -> %s(unit=%d) 期望 select_unit(unit=%d)\n", text.c_str(),
                    english_learning::IntentName(p.intent), p.unit, want_unit);
    }
}

/** 用更大的 unit_count 测（用于验证 10/11/20 这类两位数中文数字） */
void CheckUnitWide(const std::string& text, int want_unit) {
    g_total++;
    english_learning::ParsedIntent p = english_learning::ParseIntent(text, 20);
    bool ok = (p.intent == english_learning::Intent::kSelectUnit && p.unit == want_unit);
    if (!ok) {
        g_failed++;
        std::printf("  [FAIL] \"%s\" -> %s(unit=%d) 期望 select_unit(unit=%d)\n", text.c_str(),
                    english_learning::IntentName(p.intent), p.unit, want_unit);
    }
}

void CheckIntent(const std::string& text, english_learning::Intent want) {
    g_total++;
    english_learning::ParsedIntent p = english_learning::ParseIntent(text, 6);
    if (p.intent != want) {
        g_failed++;
        std::printf("  [FAIL] \"%s\" -> %s 期望 %s\n", text.c_str(),
                    english_learning::IntentName(p.intent), english_learning::IntentName(want));
    }
}

}  // namespace

int main() {
    using english_learning::Intent;

    std::printf("== intent_parser 单元测试 ==\n");

    std::printf("- 开始学习\n");
    CheckIntent("开始学习英语单词", Intent::kStart);
    CheckIntent("我想学英语单词", Intent::kStart);
    CheckIntent("背单词", Intent::kStart);
    CheckIntent("学英语", Intent::kStart);
    CheckIntent("我要背英文单词", Intent::kStart);
    CheckIntent("開始學習英語單詞", Intent::kStart);

    std::printf("- 选择单元\n");
    CheckUnit("第三单元", 3);
    CheckUnit("3单元", 3);
    CheckUnit("第3单元", 3);
    CheckUnit("第3个单元", 3);
    CheckUnit("第三個單元", 3);
    CheckUnit("学习第三单元的单词", 3);
    CheckUnit("第一单元", 1);
    CheckUnit("第六单元", 6);
    CheckUnitWide("第十单元", 10);
    CheckUnitWide("第十一单元", 11);
    CheckUnitWide("第二十单元", 20);
    CheckUnit(" unit 2 ", 2);
    CheckUnit("二单元", 2);

    std::printf("- 下一个\n");
    CheckIntent("下一个", Intent::kNext);
    CheckIntent("下一个单词", Intent::kNext);
    CheckIntent("下一个单元", Intent::kNext);
    CheckIntent("继续", Intent::kNext);
    CheckIntent("Next", Intent::kNext);

    std::printf("- 结束\n");
    CheckIntent("结束学习", Intent::kExit);
    CheckIntent("不学了", Intent::kExit);
    CheckIntent("退出", Intent::kExit);
    CheckIntent("停止学习", Intent::kExit);

    std::printf("- 列单元\n");
    CheckIntent("有哪些单元", Intent::kListUnits);
    CheckIntent("单元列表", Intent::kListUnits);
    CheckIntent("有几个单元", Intent::kListUnits);

    std::printf("- 重复\n");
    CheckIntent("重复一遍", Intent::kRepeat);
    CheckIntent("再来一次", Intent::kRepeat);

    std::printf("- 不认识\n");
    CheckIntent("今天天气怎么样", Intent::kNone);
    CheckIntent("你好", Intent::kNone);
    CheckIntent("", Intent::kNone);
    CheckIntent("!!!???", Intent::kNone);

    std::printf("- 边界\n");
    // 说了"单元"但没给合法编号 → 当作"开始"，让设备反问
    CheckIntent("单元", Intent::kStart);
    // 越界单元号 → 当作"开始"
    CheckIntent("第99单元", Intent::kStart);
    // 标点、空格、混合
    CheckUnit("第三单元。", 3);
    CheckUnit("  第三单元  ", 3);
    CheckUnit("第 三 单 元", 3);
    // "结束学习"里含"学习"，退出必须优先
    CheckIntent("结束学习吧", Intent::kExit);

    std::printf("\n断言 %d 条，失败 %d 条\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
