/**
 * @file intent_parser.cc
 * @brief 教学指令解析实现
 *
 * 解析策略（纯规则，确定性）：
 *   1. 归一化：转小写、去空白与标点、全角转半角、只保留「字母数字 + 汉字」
 *   2. 按优先级匹配关键词短语
 *   3. 「第 X 单元」里的 X 支持阿拉伯数字和中文数字（一 ~ 九十九）
 *
 * 关键词表同时覆盖简体与常见繁体写法，因为 ASR 的输出字形不固定。
 */
#include "intent_parser.h"

#include <cctype>
#include <cstdint>
#include <cstring>

namespace english_learning {
namespace {

/* ---------------- UTF-8 基础工具 ---------------- */

constexpr uint32_t kCjkBegin = 0x4E00;  // 一
constexpr uint32_t kCjkEnd = 0x9FFF;    // 鿿
constexpr uint32_t kCjkExtBegin = 0x3400;
constexpr uint32_t kCjkExtEnd = 0x4DBF;

/** 取 s[pos] 处的码点，并把 pos 推进到下一个字符起点。越界返回 0。 */
uint32_t NextCodePoint(const std::string& s, size_t& pos) {
    if (pos >= s.size()) {
        return 0;
    }
    unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c < 0x80) {
        pos += 1;
        return c;
    }
    size_t len = 1;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
        cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        cp = c & 0x07u;
    } else {
        pos += 1;
        return 0xFFFD;
    }
    if (pos + len > s.size()) {
        pos = s.size();
        return 0xFFFD;
    }
    for (size_t k = 1; k < len; k++) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[pos + k]) & 0x3Fu);
    }
    pos += len;
    return cp;
}

/** 向前回退一个字符，返回其起始位置；已经是首字符则返回 npos。 */
size_t PrevCodePointStart(const std::string& s, size_t pos) {
    if (pos == 0 || pos > s.size()) {
        return std::string::npos;
    }
    size_t p = pos - 1;
    while (p > 0 && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80) {
        p--;
    }
    return p;
}

/**
 * 只看 s[pos] 处的码点，**不修改 pos**。
 * （NextCodePoint 会推进位置，用在回溯扫描里会让下标越走越远。）
 */
uint32_t PeekCodePoint(const std::string& s, size_t pos) {
    if (pos >= s.size()) {
        return 0;
    }
    size_t p = pos;
    return NextCodePoint(s, p);
}

bool IsCjk(uint32_t cp) {
    return (cp >= kCjkBegin && cp <= kCjkEnd) || (cp >= kCjkExtBegin && cp <= kCjkExtEnd);
}

/* ---------------- 归一化 ---------------- */

/**
 * 只保留「ASCII 字母数字」和「汉字」，其余（空白、标点、符号）全部丢弃；
 * ASCII 字母转小写；全角字母数字转半角。
 */
std::string Normalize(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t start = pos;
        uint32_t cp = NextCodePoint(text, pos);
        if (cp == 0) {
            break;
        }
        if (cp < 0x80) {
            if (cp >= '0' && cp <= '9') {
                out += static_cast<char>(cp);
            } else if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) {
                out += static_cast<char>(std::tolower(static_cast<int>(cp)));
            }
            continue;
        }
        // 全角 → 半角
        if (cp >= 0xFF01 && cp <= 0xFF5E) {
            uint32_t half = cp - 0xFEE0u;
            if (half >= '0' && half <= '9') {
                out += static_cast<char>(half);
            } else if ((half >= 'a' && half <= 'z') || (half >= 'A' && half <= 'Z')) {
                out += static_cast<char>(std::tolower(static_cast<int>(half)));
            }
            continue;
        }
        if (IsCjk(cp)) {
            out.append(text, start, pos - start);
        }
        // 其它（标点、IPA、emoji…）一律丢弃
    }
    return out;
}

bool Contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

/* ---------------- 中文数字 ---------------- */

/** 中文数字 → 数值；不是数字返回 -1 */
int ChineseDigitValue(uint32_t cp) {
    switch (cp) {
        case 0x96F6: return 0;   // 零
        case 0x4E00: return 1;   // 一
        case 0x4E8C: return 2;   // 二
        case 0x4E24: return 2;   // 兩
        case 0x4E09: return 3;   // 三
        case 0x56DB: return 4;   // 四
        case 0x4E94: return 5;   // 五
        case 0x516D: return 6;   // 六
        case 0x4E03: return 7;   // 七
        case 0x516B: return 8;   // 八
        case 0x4E5D: return 9;   // 九
        case 0x5341: return 10;  // 十
        default: return -1;
    }
}

/** 解析一段中文数字（支持 一 ~ 九十九） */
int ParseChineseNumber(const std::string& s) {
    int total = 0;
    int current = 0;
    size_t pos = 0;
    while (pos < s.size()) {
        uint32_t cp = NextCodePoint(s, pos);
        int v = ChineseDigitValue(cp);
        if (v < 0) {
            continue;
        }
        if (v == 10) {
            if (current == 0) {
                current = 1;  // 「十」= 10
            }
            current *= 10;
        } else {
            if (current >= 10) {
                total += current;
                current = 0;
            }
            current += v;
        }
    }
    return total + current;
}

/** 解析一段数字（阿拉伯数字优先，否则按中文数字） */
int ParseNumberSegment(const std::string& s) {
    if (s.empty()) {
        return 0;
    }
    bool all_ascii_digit = true;
    for (char c : s) {
        if (c < '0' || c > '9') {
            all_ascii_digit = false;
            break;
        }
    }
    if (all_ascii_digit) {
        int v = 0;
        for (char c : s) {
            v = v * 10 + (c - '0');
        }
        return v;
    }
    return ParseChineseNumber(s);
}

/**
 * 从 from 开始向右扫一段连续数字（阿拉伯数字或中文数字），返回结束位置。
 * 没有数字时返回 from。
 */
size_t ScanNumberForward(const std::string& s, size_t from) {
    size_t p = from;
    while (p < s.size()) {
        uint32_t cp = PeekCodePoint(s, p);
        bool is_digit = (cp >= '0' && cp <= '9') || ChineseDigitValue(cp) >= 0;
        if (!is_digit) {
            break;
        }
        NextCodePoint(s, p);  // 推进到下一个字符
    }
    return p;
}

/** 从 from 开始向左扫一段连续数字，返回起始位置。 */
size_t ScanNumberBackward(const std::string& s, size_t from) {
    size_t start = from;
    while (start > 0) {
        size_t prev = PrevCodePointStart(s, start);
        if (prev == std::string::npos) {
            break;
        }
        uint32_t cp = PeekCodePoint(s, prev);
        bool is_digit = (cp >= '0' && cp <= '9') || ChineseDigitValue(cp) >= 0;
        if (!is_digit) {
            break;
        }
        start = prev;
    }
    return start;
}

/** 在 marker（"单元"的起点）之前找单元号，允许跨过一个「个 / 個」。 */
int ExtractNumberBefore(const std::string& s, size_t marker) {
    size_t p = marker;
    for (int hop = 0; hop < 2; hop++) {
        size_t q = PrevCodePointStart(s, p);
        if (q == std::string::npos) {
            return 0;
        }
        uint32_t cp = PeekCodePoint(s, q);
        bool is_digit = (cp >= '0' && cp <= '9') || ChineseDigitValue(cp) >= 0;
        if (!is_digit) {
            // 只允许跳过「个 / 個」
            if (cp == 0x4E2A || cp == 0x500B) {
                p = q;
                continue;
            }
            return 0;
        }
        size_t start = ScanNumberBackward(s, q);
        size_t end = ScanNumberForward(s, q);
        if (end > marker) {
            end = marker;
        }
        return ParseNumberSegment(s.substr(start, end - start));
    }
    return 0;
}

/** 在 pos 之后找单元号（英文 "unit2" 形式，数字在后）。 */
int ExtractNumberAfter(const std::string& s, size_t pos) {
    size_t end = ScanNumberForward(s, pos);
    if (end == pos) {
        return 0;
    }
    return ParseNumberSegment(s.substr(pos, end - pos));
}

/**
 * 抠出单元号。
 * 支持：第三单元 / 3单元 / 第3个单元 / 第三個單元 / unit2
 */
int ExtractUnitNumber(const std::string& norm) {
    size_t marker = norm.find("单元");
    if (marker == std::string::npos) {
        marker = norm.find("單元");
    }
    if (marker != std::string::npos) {
        int v = ExtractNumberBefore(norm, marker);
        if (v > 0) {
            return v;
        }
    }
    // 英文形式：unit2 / unit 3（归一化后空格已去掉）
    size_t um = norm.find("unit");
    if (um != std::string::npos) {
        int v = ExtractNumberAfter(norm, um + 4);
        if (v > 0) {
            return v;
        }
    }
    return 0;
}

/* ---------------- 关键词表 ---------------- */

struct KeywordSet {
    const char* const* items;
    size_t count;
};

bool HasAny(const std::string& norm, const KeywordSet& set) {
    for (size_t i = 0; i < set.count; i++) {
        if (Contains(norm, set.items[i])) {
            return true;
        }
    }
    return false;
}

const char* const kExitWords[] = {
    "结束学习", "結束學習", "结束", "結束", "不学了", "不學了", "不背了", "不背了",
    "退出学习", "退出學習", "退出", "别学了", "別學了", "停止学习", "停止學習", "停止",
    "关闭学习", "關閉學習", "不练了", "不練了",
};

const char* const kListUnitsWords[] = {
    "有哪些单元", "有哪些單元", "有什么单元", "有什麼單元", "单元列表", "單元列表",
    "有几个单元", "有几个單元", "多少单元", "多少單元", "单元都有什么", "列出单元",
    "列出單元", "单元一览", "單元一覽", "单元介绍",
};

const char* const kNextWords[] = {
    "下一个", "下一個", "下一个单词", "下一个单词", "下一个词", "下一課", "下一课",
    "继续", "繼續", "next", "再下一个", "下一个吧",
};

const char* const kStartWords[] = {
    "英语单词", "英語單詞", "英文单词", "英文單詞", "背单词", "背單詞", "学单词", "學單詞",
    "背英语", "背英語", "学英语", "學英語", "背英文", "学英文", "学习单词", "學習單詞",
    "学习英语", "學習英語", "英语学习", "english", "开始学习", "開始學習",
};

const char* const kRepeatWords[] = {
    "重复", "重複", "重复一遍", "再来一次", "再來一次", "再读一遍", "再讀一遍", "重读",
    "再听一遍", "再聽一遍", "没听清", "沒聽清", "没听清", "再说一遍", "再說一遍",
};

const KeywordSet kExitSet{kExitWords, sizeof(kExitWords) / sizeof(kExitWords[0])};
const KeywordSet kListUnitsSet{kListUnitsWords, sizeof(kListUnitsWords) / sizeof(kListUnitsWords[0])};
const KeywordSet kNextSet{kNextWords, sizeof(kNextWords) / sizeof(kNextWords[0])};
const KeywordSet kStartSet{kStartWords, sizeof(kStartWords) / sizeof(kStartWords[0])};
const KeywordSet kRepeatSet{kRepeatWords, sizeof(kRepeatWords) / sizeof(kRepeatWords[0])};

}  // namespace

ParsedIntent ParseIntent(const std::string& text, int unit_count) {
    ParsedIntent result;
    if (text.empty()) {
        return result;
    }
    std::string norm = Normalize(text);
    if (norm.empty()) {
        return result;
    }

    // 1) 退出 —— 放最前，"结束学习" 里也含"学习"
    if (HasAny(norm, kExitSet)) {
        result.intent = Intent::kExit;
        return result;
    }
    // 2) 列单元
    if (HasAny(norm, kListUnitsSet)) {
        result.intent = Intent::kListUnits;
        return result;
    }
    // 3) 下一个 —— 必须排在选单元前面，"下一个单元"是"下一个"不是"第1单元"
    if (HasAny(norm, kNextSet)) {
        result.intent = Intent::kNext;
        return result;
    }
    // 4) 选单元 —— 必须排在"开始"前面，"学习第三单元的单词"是选单元
    //    中文说"单元"，英文说"unit"，两种都要认
    const bool mentions_unit_zh = Contains(norm, "单元") || Contains(norm, "單元");
    const bool mentions_unit_en = Contains(norm, "unit");
    if (mentions_unit_zh || mentions_unit_en) {
        int unit = ExtractUnitNumber(norm);
        if (unit > 0 && unit <= unit_count) {
            result.intent = Intent::kSelectUnit;
            result.unit = unit;
            return result;
        }
        // 说了"单元"但没解析出合法编号：当作"开始学习"，让设备反问
        if (mentions_unit_zh) {
            result.intent = Intent::kStart;
            return result;
        }
        // 英文 "unit" 后面没数字 → 不认，继续往下
    }
    // 5) 开始学习
    if (HasAny(norm, kStartSet)) {
        result.intent = Intent::kStart;
        return result;
    }
    // 6) 重复
    if (HasAny(norm, kRepeatSet)) {
        result.intent = Intent::kRepeat;
        return result;
    }
    return result;
}

const char* IntentName(Intent intent) {
    switch (intent) {
        case Intent::kStart: return "start";
        case Intent::kSelectUnit: return "select_unit";
        case Intent::kNext: return "next";
        case Intent::kRepeat: return "repeat";
        case Intent::kExit: return "exit";
        case Intent::kListUnits: return "list_units";
        default: return "none";
    }
}

}  // namespace english_learning
