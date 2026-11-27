/**
 * @file word_match.h
 * @brief 跟读判定：把孩子的 ASR 文本和目标单词比对
 *
 * 背景
 * ----
 * 设备端拿不到孩子的原始音频 —— 录音上行给服务器做 ASR，设备只收到识别后的文字，
 * 而且消息里没有置信度（type="stt" 只有 text 字段）。
 * 所以「读得对不对」只能用 **ASR 文本 vs 目标词** 来判断。
 *
 * 判定必须宽松
 * ------------
 * 儿童发音的 ASR 识别率低，读对了也可能被识别成别的词。规则按这个前提设计：
 *  - 只有「明显不对」才判 kWrong；
 *  - 空白、纯语气词、纯中文一律判 kEmpty（当没听清，不算孩子读错）；
 *  - 差一两个字母、或漏读/多说一个词，判 kClose（算读对，只是没读准）；
 *  - 目标词和识别文本用同一套规则归一化，礼貌语和语气词两边都剔掉。
 *
 * 这里是纯逻辑，不依赖任何平台头文件，方便在主机侧做表驱动测试。
 */
#ifndef ENGLISH_WORD_MATCH_H_
#define ENGLISH_WORD_MATCH_H_

#include <string>

namespace english_learning {

/** 跟读判定结果 */
enum class MatchResult {
    kEmpty = 0, /**< 没听清：空白、语气词或纯中文，不算读错 */
    kExact = 1, /**< 完全对上 */
    kClose = 2, /**< 接近：差一两个字母，或漏读/多说一个词 */
    kWrong = 3, /**< 明显不对 */
};

/** 判定结果的可读名字（调试日志用） */
const char* MatchResultName(MatchResult result);

/**
 * @brief 判断孩子读的 spoken 算不算读对了目标词 target
 *
 * @param target 目标单词或短语，如 "afternoon"、"office worker"
 * @param spoken  ASR 识别出来的文本，如 "Afternoon!"、"嗯 afternoon"
 * @return kEmpty / kExact / kClose / kWrong
 */
MatchResult MatchSpoken(const std::string& target, const std::string& spoken);

/**
 * @brief 这个词是不是语气词/礼貌语（比对前要剔掉）
 *
 * 单独暴露出来，是为了让单元测试能检查这张表跟词表没有冲突 ——
 * 词表里真有一个叫 "now" 的词的话，它绝不能出现在这张表里。
 */
bool IsFillerWord(const std::string& word);

}  // namespace english_learning

#endif  // ENGLISH_WORD_MATCH_H_
