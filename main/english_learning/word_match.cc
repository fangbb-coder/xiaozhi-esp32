/**
 * @file word_match.cc
 * @brief 跟读判定的实现
 */

#include "word_match.h"

#include <cstddef>
#include <vector>

namespace english_learning {

namespace {

/**
 * @brief 语气词 / 礼貌语
 *
 * 比对前两边都剔掉这些词，否则「嗯 afternoon」「afternoon please」这类
 * 常见输出会被判成不匹配。
 *
 * 约束：**这张表不能命中词表里的任何真实单词**。
 * 单元测试会拿 kEnglishUnits 全表扫一遍来守住这条约束
 * （所以 "now" 不能进这张表 —— 词表 Unit 4 里真有一个 now）。
 */
const char* const kFillerWords[] = {
    "um", "uh", "er", "ah", "hmm", "mm", "eh",
    "the", "a", "an",
    "please", "thanks", "thank", "you",
    "ok", "okay", "yeah", "yes",
    "and", "so", "just", "really",
    "i", "im", "my", "me",
};

/**
 * @brief 归一化：只保留 ASCII 字母，其余全变成空格
 *
 * 中文、标点、数字都被丢掉。这有两个好处：
 *  1) 孩子说「下午 afternoon」也能匹配上；
 *  2) 孩子说「我不会说」会变成空串，于是判成 kEmpty 而不是 kWrong ——
 *     「不会说」不是「读错」，不该被纠正。
 */
std::string Normalize(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
        } else if (c >= 'a' && c <= 'z') {
            out += c;
        } else {
            out += ' ';
        }
    }
    return out;
}

/** 按空格切成词，顺便压掉连续空格 */
std::vector<std::string> SplitWords(const std::string& text, bool drop_fillers) {
    std::vector<std::string> words;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') {
            i++;
        }
        size_t start = i;
        while (i < text.size() && text[i] != ' ') {
            i++;
        }
        if (i > start) {
            std::string word = text.substr(start, i - start);
            if (!drop_fillers || !IsFillerWord(word)) {
                words.push_back(word);
            }
        }
    }
    return words;
}

/** 编辑距离的缓冲上限。词表里最长的词是 14 字符，ASR 输出再离谱也够用。 */constexpr size_t kMaxWordLen = 31;/** 编辑距离（Levenshtein）。词很短，O(n*m) 完全够用。 */
int Levenshtein(const std::string& a, const std::string& b) {
    const size_t n = a.size();
    const size_t m = b.size();
    if (n == 0) {
        return static_cast<int>(m);
    }
    if (m == 0) {
        return static_cast<int>(n);
    }
    if (n > kMaxWordLen || m > kMaxWordLen) {
        // 词超长（ASR 输出了完全离谱的东西），直接当成完全不同
        return (a == b) ? 0 : kMaxWordLen + 1;
    }
    int prev[kMaxWordLen + 1];
    int cur[kMaxWordLen + 1];
    for (size_t j = 0; j <= m; j++) {
        prev[j] = static_cast<int>(j);
    }
    for (size_t i = 1; i <= n; i++) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= m; j++) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int best = prev[j] + 1;
            if (cur[j - 1] + 1 < best) {
                best = cur[j - 1] + 1;
            }
            if (prev[j - 1] + cost < best) {
                best = prev[j - 1] + cost;
            }
            cur[j] = best;
        }
        for (size_t j = 0; j <= m; j++) {
            prev[j] = cur[j];
        }
    }
    return prev[m];
}

/**
 * @brief 比较两串词
 *
 * 允许的误差（刻意宽松，因为是给小孩用）：
 *  - 词数相同：总编辑距离 ≤1（单词）或 ≤2（短语）→ kClose；
 *  - 词数差 1：只能是漏读或多说了最后一个词 → kClose。
 */
MatchResult CompareWordLists(const std::vector<std::string>& target,
                              const std::vector<std::string>& spoken) {
    const size_t n = target.size();
    const size_t m = spoken.size();
    if (n == 0 || m == 0) {
        return MatchResult::kEmpty;
    }

    if (n == m) {
        int total = 0;
        for (size_t i = 0; i < n; i++) {
            total += Levenshtein(target[i], spoken[i]);
        }
        if (total == 0) {
            return MatchResult::kExact;
        }
        // 单词容 2 个字母、短语容 3 个：儿童发音的系统性偏差（双元音读偏等）
        // 不该被判成「明显错误」——用户的原话是「明显错误才纠正」。
        const int allowance = (n == 1) ? 2 : 3;
        return total <= allowance ? MatchResult::kClose : MatchResult::kWrong;
    }

    // 词数差 1：只可能是漏读或多说了最后一个词
    if (m + 1 == n || m == n + 1) {
        const size_t common = (m < n) ? m : n;
        for (size_t i = 0; i < common; i++) {
            // 末尾那个对齐的词允许差 2 个字母（少读时通常只差词尾）
            const int limit = (i + 1 == common) ? 2 : 0;
            if (Levenshtein(target[i], spoken[i]) > limit) {
                return MatchResult::kWrong;
            }
        }
        return MatchResult::kClose;
    }

    return MatchResult::kWrong;
}

}  // namespace

const char* MatchResultName(MatchResult result) {
    switch (result) {
        case MatchResult::kEmpty:
            return "empty";
        case MatchResult::kExact:
            return "exact";
        case MatchResult::kClose:
            return "close";
        case MatchResult::kWrong:
            return "wrong";
        default:
            return "?";
    }
}

bool IsFillerWord(const std::string& word) {
    for (const char* filler : kFillerWords) {
        if (word == filler) {
            return true;
        }
    }
    return false;
}

MatchResult MatchSpoken(const std::string& target, const std::string& spoken) {
    std::vector<std::string> target_words = SplitWords(Normalize(target), true);
    if (target_words.empty()) {
        // 目标词本身被剔空了（词表里不会发生），退回不剔除的结果
        target_words = SplitWords(Normalize(target), false);
    }
    std::vector<std::string> spoken_words = SplitWords(Normalize(spoken), true);
    return CompareWordLists(target_words, spoken_words);
}

}  // namespace english_learning
