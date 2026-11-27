/**
 * @file word_match_test.cc
 * @brief 跟读判定（ASR 文本 vs 目标词）的主机侧单元测试
 *
 * 判定要宽松是故意的 —— 儿童发音的 ASR 识别率低，宁可放过也不能
 * 老是判孩子读错。所以这里的重点是：
 *  - 明显不对必须判 kWrong；
 *  - 没听清（空/语气词/纯中文）必须判 kEmpty，不能当成读错；
 *  - 差一两个字母、少读或多说一个词都要算 kClose（算读对）。
 *
 * 编译见 tools/host-syntax 下的说明。
 */

#include <cstdio>
#include <string>

#include "word_match.h"
#include "word_table.h"

using namespace english_learning;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                      \
    } while (0)

/** 断言某个读法算不算读对 */
void Expect(const char* target, const char* spoken, MatchResult want, const char* label) {
    const MatchResult got = MatchSpoken(target, spoken);
    g_checks++;
    if (got != want) {
        g_failures++;
        printf("FAIL %s: MatchSpoken(\"%s\", \"%s\") = %s, want %s\n", label, target,
               spoken, MatchResultName(got), MatchResultName(want));
    }
}

void TestExact() {
    Expect("afternoon", "afternoon", MatchResult::kExact, "exact/plain");
    Expect("afternoon", "Afternoon", MatchResult::kExact, "exact/upper-first");
    Expect("afternoon", "AFTERNOON", MatchResult::kExact, "exact/upper");
    Expect("afternoon", "afternoon.", MatchResult::kExact, "exact/dot");
    Expect("doctor", "Doctor!", MatchResult::kExact, "exact/bang");
    Expect("office worker", "office worker", MatchResult::kExact, "exact/phrase");
    Expect("police officer", "police officer", MatchResult::kExact, "exact/phrase2");
    Expect("T-shirt", "T-shirt", MatchResult::kExact, "exact/hyphen");
}

void TestNormalize() {
    Expect("afternoon", "  afternoon  ", MatchResult::kExact, "norm/spaces");
    Expect("afternoon", "a-f-t-e-r-n-o-o-n", MatchResult::kWrong, "norm/spelled-out-is-wrong");
    Expect("T-shirt", "t-shirt", MatchResult::kExact, "norm/hyphen-case");
    Expect("T-shirt", "t shirt", MatchResult::kExact, "norm/hyphen-vs-space");
    Expect("office worker", "office  worker", MatchResult::kExact, "norm/double-space");
    Expect("afternoon", "after\nnoon", MatchResult::kWrong, "norm/split-word-is-wrong");
}

void TestFiller() {
    Expect("afternoon", "um afternoon", MatchResult::kExact, "filler/leading-um");
    Expect("afternoon", "afternoon please", MatchResult::kExact, "filler/trailing-please");
    Expect("afternoon", "yeah afternoon", MatchResult::kExact, "filler/leading-yeah");
    Expect("afternoon", "the afternoon", MatchResult::kExact, "filler/leading-the");
    Expect("afternoon", "下午 afternoon", MatchResult::kExact, "filler/chinese-prefix");
    Expect("afternoon", "afternoon 谢谢", MatchResult::kExact, "filler/chinese-suffix");
    Expect("a lot of", "a lot of", MatchResult::kExact, "filler/phrase-with-a");
    Expect("a lot of", "lot of", MatchResult::kExact, "filler/phrase-a-dropped");
    Expect("make the bed", "make the bed", MatchResult::kExact, "filler/phrase-with-the");
    Expect("make the bed", "make bed", MatchResult::kExact, "filler/phrase-the-dropped");
    Expect("doctor", "doctor and", MatchResult::kExact, "filler/trailing-and");
}

void TestEmpty() {
    Expect("afternoon", "", MatchResult::kEmpty, "empty/blank");
    Expect("afternoon", "   ", MatchResult::kEmpty, "empty/spaces");
    Expect("afternoon", "um", MatchResult::kEmpty, "empty/um-only");
    Expect("afternoon", "嗯", MatchResult::kEmpty, "empty/chinese-only");
    Expect("afternoon", "我不会说", MatchResult::kEmpty, "empty/chinese-sentence");
    Expect("afternoon", "？", MatchResult::kEmpty, "empty/punct-only");
    Expect("afternoon", "the a an", MatchResult::kEmpty, "empty/fillers-only");
}

void TestClose() {
    Expect("afternoon", "afternigt", MatchResult::kWrong, "close/three-letters");
    Expect("afternoon", "afternign", MatchResult::kClose, "close/one-letter");
    Expect("afternoon", "afternoonn", MatchResult::kClose, "close/one-letter-extra");
    Expect("doctor", "doktor", MatchResult::kClose, "close/pronunciation");
    Expect("office worker", "office work", MatchResult::kClose, "close/missing-last-word");
    Expect("police officer", "police office", MatchResult::kClose, "close/last-word-tail");
    Expect("look after", "look", MatchResult::kClose, "close/missing-word");
    Expect("delivery worker", "delivery worker extra", MatchResult::kClose, "close/extra-word");
    Expect("community", "community college", MatchResult::kClose, "close/extra-word2");
}

void TestWrong() {
    Expect("afternoon", "monkey", MatchResult::kWrong, "wrong/other-word");
    Expect("doctor", "teacher", MatchResult::kWrong, "wrong/other-word2");
    Expect("afternoon", "mon", MatchResult::kWrong, "wrong/too-short");
    Expect("playground", "play ground", MatchResult::kWrong, "wrong/split-word");
    Expect("basketball", "basket", MatchResult::kWrong, "wrong/partial");
    Expect("community", "communty", MatchResult::kClose, "wrong/one-letter");
    Expect("community", "commuity", MatchResult::kClose, "wrong/two-letters-close");
    Expect("community", "commty", MatchResult::kWrong, "wrong/three-letters");
    Expect("office worker", "office manager", MatchResult::kWrong, "wrong/other-phrase");
    Expect("afternoon", "afternoon afternoon", MatchResult::kClose, "wrong/repeat-is-close");
}

/**
 * @brief 语气词表不能命中词表里的任何真实单词
 *
 * 用 MatchSpoken(w, w) == kExact 来守住这条约束：如果某个词恰好在语气词表里，
 * 它会在归一化时被剔掉，两边都变空 → kEmpty，断言就挂了。
 */
void TestFillerTableHasNoConflictWithWordList() {
    int total = 0;
    for (int u = 0; u < static_cast<int>(kEnglishUnitCount); u++) {
        const UnitEntry& unit = kEnglishUnits[u];
        for (int i = 0; i < static_cast<int>(unit.word_count); i++) {
            const std::string word = unit.words[i].en;
            total++;
            const MatchResult got = MatchSpoken(word, word);
            g_checks++;
            if (got != MatchResult::kExact) {
                g_failures++;
                printf("FAIL wordlist: MatchSpoken(\"%s\", \"%s\") = %s, want exact\n",
                       word.c_str(), word.c_str(), MatchResultName(got));
            }
        }
    }
    g_checks++;
    if (total != 110) {
        g_failures++;
        printf("FAIL wordlist: expected 110 words, got %d\n", total);
    }
}

void TestEmptyTarget() {
    Expect("", "afternoon", MatchResult::kEmpty, "target/empty");
    Expect("   ", "afternoon", MatchResult::kEmpty, "target/spaces");
}

}  // namespace

int main() {
    TestExact();
    TestNormalize();
    TestFiller();
    TestEmpty();
    TestClose();
    TestWrong();
    TestFillerTableHasNoConflictWithWordList();
    TestEmptyTarget();

    printf("\n断言 %d 条，失败 %d 条\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
