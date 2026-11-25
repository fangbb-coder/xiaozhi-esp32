/**
 * @file intent_parser.h
 * @brief 教学指令解析：把用户说的一句话（ASR 文本）解析成一个意图
 *
 * 设计约束：
 *  - **纯 C++，不依赖 ESP-IDF / 不依赖 STL 容器**，只允许 <string>。
 *    这样它既能在固件里跑，也能在主机侧（gcc/clang）单独编译做单元测试。
 *  - 不做语义理解，只做确定性的规则匹配。教学流程是固定剧本，规则比 LLM 更可靠。
 */
#ifndef ENGLISH_LEARNING_INTENT_PARSER_H_
#define ENGLISH_LEARNING_INTENT_PARSER_H_

#include <string>

namespace english_learning {

/** 用户一句话对应的意图 */
enum class Intent {
    kNone = 0,     /**< 不认识，交给正常对话流程 */
    kStart,        /**< 「开始学习英语单词」→ 进入选单元 */
    kSelectUnit,   /**< 「第三单元」→ 开始学某个单元 */
    kNext,         /**< 「下一个」→ 推进到下一个词 */
    kRepeat,       /**< 「重复一遍」→ 重播当前词 */
    kExit,         /**< 「结束学习」→ 退出教学模式 */
    kListUnits,    /**< 「有哪些单元」→ 列出单元 */
};

/** 解析结果 */
struct ParsedIntent {
    Intent intent = Intent::kNone;
    int unit = 0;  /**< 仅 kSelectUnit 有效，1..unit_count */
};

/**
 * @brief 解析一句话
 *
 * @param text        ASR 识别出的原文（可能有标点、空格、大小写混杂）
 * @param unit_count  词表里的单元总数，用来校验「第 9 单元」这种越界请求
 * @return 解析结果；不认识时 intent == Intent::kNone
 */
ParsedIntent ParseIntent(const std::string& text, int unit_count);

/** 意图名（调试用） */
const char* IntentName(Intent intent);

}  // namespace english_learning

#endif  // ENGLISH_LEARNING_INTENT_PARSER_H_
