/**
 * @file english_learning_test.cc
 * @brief 英语学习模块的主机侧单元测试
 *
 * 覆盖两件事：
 *  1) 音频查表的「多候选弱符号」回退机制（候选 0 / 候选 1 各命中一次）
 *  2) 教学状态机的完整剧本：开始 → 选单元 → 逐词 → 结束
 *
 * 编译见 tools/host-syntax 下的说明。
 */

#include <cstdio>
#include <string>
#include <vector>

#include "english_audio.h"
#include "english_learning.h"
#include "intent_parser.h"
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

// 假的「编进固件」的音频由 fake_audio.S 提供，布局照抄 ESP-IDF 的 EMBED_FILES：
// _binary_xxx_start 紧挨着 _binary_xxx_end。
// 候选 0 是 _binary_<文件名>_start，候选 1 是 _binary_assets_english_<文件名>_start。
extern "C" {
extern const char fake_c0_start[] asm("_binary_en_u1_w01_ogg_start");
extern const char fake_c0_end[] asm("_binary_en_u1_w01_ogg_end");
extern const char fake_c1_start[] asm("_binary_assets_english_en_u1_w02_ogg_start");
extern const char fake_c1_end[] asm("_binary_assets_english_en_u1_w02_ogg_end");
}  // extern "C"

/** 记录所有钩子调用 */
struct Recorder {
    std::vector<std::string> played;
    std::vector<StudyCard> cards;
    std::vector<std::string> texts;
    std::vector<bool> listening;
    std::vector<bool> suppress;
    std::vector<std::string> idles;
    bool suppress_now = false;

    void Clear() {
        played.clear();
        cards.clear();
        texts.clear();
        listening.clear();
        suppress.clear();
        idles.clear();
    }
};

Recorder g_rec;

void InstallHooks() {
    EnglishLearning::Hooks hooks;
    hooks.play_asset = [](const std::string& name) {
        printf("    [play] %s\n", name.c_str());
        g_rec.played.push_back(name);
    };
    hooks.show_card = [](const StudyCard& card) { g_rec.cards.push_back(card); };
    hooks.show_text = [](const std::string& text) { g_rec.texts.push_back(text); };
    hooks.set_listening = [](bool start) { g_rec.listening.push_back(start); };
    hooks.suppress_server_speech = [](bool suppress) {
        g_rec.suppress.push_back(suppress);
        g_rec.suppress_now = suppress;
    };
    hooks.set_idle = [](const std::string& text) { g_rec.idles.push_back(text); };
    EnglishLearning::GetInstance().SetHooks(std::move(hooks));
}

/** 模拟一次「播放队列排空」 */
void Drain() { EnglishLearning::GetInstance().OnPlaybackFinished(); }

void TestAudioResolver() {
    printf("--- 音频查表 ---\n");
    EnglishAudio audio;

    CHECK(GetEnglishAudio("en_u1_w01.ogg", &audio));
    CHECK(audio.data == fake_c0_start);
    CHECK(audio.size == 8);

    // 候选 1（相对组件目录）也要能命中
    CHECK(GetEnglishAudio("en_u1_w02.ogg", &audio));
    CHECK(audio.data == fake_c1_start);
    CHECK(audio.size == 8);

    // 三个候选都没定义的资源取不到
    CHECK(!GetEnglishAudio("en_u1_w03.ogg", &audio));

    // 不存在的名字
    CHECK(!GetEnglishAudio("no_such_file.ogg", &audio));

    // 自检：至少有一个候选能解析
    CHECK(EnglishAudioCandidateIndex() >= 0);
}

void TestIntentParserStillGreen() {
    printf("--- 意图解析 ---\n");
    CHECK(ParseIntent("开始学习英语单词", 6).intent == Intent::kStart);
    CHECK(ParseIntent("第三单元", 6).intent == Intent::kSelectUnit);
    CHECK(ParseIntent("第三单元", 6).unit == 3);
    CHECK(ParseIntent("下一个", 6).intent == Intent::kNext);
    CHECK(ParseIntent("上一个", 6).intent == Intent::kPrev);
    CHECK(ParseIntent("前一个", 6).intent == Intent::kPrev);
    CHECK(ParseIntent("第三个", 6).intent == Intent::kGoto);
    CHECK(ParseIntent("第三个", 6).word_index == 3);
    CHECK(ParseIntent("第5个", 6).intent == Intent::kGoto);
    CHECK(ParseIntent("第5个", 6).word_index == 5);
    CHECK(ParseIntent("第一个", 6).intent == Intent::kGoto);
    CHECK(ParseIntent("第一个", 6).word_index == 1);
    CHECK(ParseIntent("最后一个", 6).intent == Intent::kGoto);
    CHECK(ParseIntent("最后一个", 6).word_index == -1);
    // 「第三个单元」是选单元，不是跳到第 3 个词
    CHECK(ParseIntent("第三个单元", 6).intent == Intent::kSelectUnit);
    CHECK(ParseIntent("第三个单元", 6).unit == 3);
    CHECK(ParseIntent("结束学习", 6).intent == Intent::kExit);
    CHECK(ParseIntent("有哪些单元", 6).intent == Intent::kListUnits);
    CHECK(ParseIntent("今天天气怎么样", 6).intent == Intent::kNone);
}

void TestFullScript() {
    printf("--- 教学剧本 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    // 0) 空闲状态下的普通闲聊不该被接管
    CHECK(!el.HandleUserText("今天天气怎么样"));
    CHECK(el.state() == State::kIdle);
    CHECK(g_rec.played.empty());

    // 1) 「开始学习英语单词」
    CHECK(el.HandleUserText("开始学习英语单词"));
    CHECK(el.state() == State::kChoosingUnit);
    // 一次只推一段音频，靠播放结束回调推进
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "prompt_start.ogg");
    // 教学期间屏蔽服务端播报
    CHECK(g_rec.suppress_now);

    // 2) 第一段放完 -> 播第二段
    g_rec.played.clear();
    Drain();
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "prompt_ask_unit.ogg");

    // 3) 第二段放完 -> 回到聆听
    g_rec.played.clear();
    Drain();
    CHECK(g_rec.played.empty());
    CHECK(g_rec.listening.size() == 1);
    CHECK(g_rec.listening.back());

    // 4) 「第三单元」
    g_rec.Clear();
    CHECK(el.HandleUserText("第三单元"));
    CHECK(el.state() == State::kStudying);
    CHECK(el.current_unit() == 3);
    CHECK(el.current_index() == 0);
    // 屏幕卡片
    CHECK(g_rec.cards.size() == 1);
    CHECK(g_rec.cards[0].unit == 3);
    CHECK(g_rec.cards[0].index == 0);
    CHECK(g_rec.cards[0].word_count == 18);
    CHECK(std::string(g_rec.cards[0].word) == "afternoon");
    CHECK(std::string(g_rec.cards[0].zh) == "下午");
    CHECK(std::string(g_rec.cards[0].spell) == "a-f-t-e-r-n-o-o-n");
    CHECK(std::string(g_rec.cards[0].syllable) == "af-ter-noon");
    CHECK(!g_rec.cards[0].is_last);
    // 播报顺序：词 -> 拼写 -> 音节拼读 -> 中文（afternoon 是多音节词，多一段）
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "en_u3_w01.ogg");
    g_rec.played.clear();
    Drain();
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "en_u3_w01_spell.ogg");
    g_rec.played.clear();
    Drain();
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "en_u3_w01_syll.ogg");
    g_rec.played.clear();
    Drain();
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "zh_u3_w01.ogg");

    // 4b) 全部放完 -> 回到聆听（还没到最后一个词）
    g_rec.Clear();
    Drain();
    CHECK(g_rec.played.empty());
    CHECK(g_rec.listening.back());

    // 5) 「下一个」
    g_rec.Clear();
    CHECK(el.HandleUserText("下一个"));
    CHECK(el.current_index() == 1);
    CHECK(g_rec.cards.size() == 1);
    CHECK(g_rec.cards[0].index == 1);
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "en_u3_w02.ogg");
    // 放完三段
    Drain();
    Drain();
    Drain();
    CHECK(el.current_index() == 1);

    // 6) 「重复一遍」：先说提示语，再重播当前词
    g_rec.Clear();
    CHECK(el.HandleUserText("重复一遍"));
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "prompt_repeat.ogg");
    g_rec.Clear();
    Drain();
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "en_u3_w02.ogg");
    g_rec.Clear();
    Drain();
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "en_u3_w02_spell.ogg");
    g_rec.Clear();
    Drain();
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "zh_u3_w02.ogg");
    Drain();

    // 7) 一路推到倒数第二个词
    //    段数随词而变：多音节词多一段音节拼读（单词/拼写/音节/中文 = 4 段）
    for (int i = 2; i < 17; i++) {
        g_rec.Clear();
        CHECK(el.HandleUserText("下一个"));
        CHECK(el.current_index() == i);
        const WordEntry& w = kEnglishUnits[2].words[i];
        const int segments = (w.syllable != nullptr && w.syllable[0] != '\0') ? 4 : 3;
        for (int s = 0; s < segments; s++) {
            Drain();
        }
    }
    CHECK(el.current_index() == 16);

    // 8) 最后一个词：播完三段后自动播「本单元单词学习结束」
    g_rec.Clear();
    CHECK(el.HandleUserText("下一个"));
    CHECK(el.current_index() == 17);
    CHECK(g_rec.cards.size() == 1);
    CHECK(g_rec.cards[0].is_last);
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "en_u3_w18.ogg");
    g_rec.Clear();
    Drain();
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "en_u3_w18_spell.ogg");
    g_rec.Clear();
    Drain();
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "zh_u3_w18.ogg");
    g_rec.Clear();
    Drain();
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "prompt_unit_done.ogg");
    CHECK(el.state() == State::kIdle);
    // 结束语还在放，先不急着放服务端

    // 9) 结束语放完 -> 回到待机（等唤醒词），且不再屏蔽服务端
    g_rec.Clear();
    Drain();
    CHECK(g_rec.played.empty());
    CHECK(g_rec.idles.size() == 1);
    CHECK(g_rec.idles.back() == "本单元单词学习结束");
    // 待机态不应该继续聆听，要等用户说唤醒词
    CHECK(g_rec.listening.empty());
    CHECK(!g_rec.suppress_now);

    // 10) 「结束学习」在空闲状态下不该被接管
    CHECK(!el.HandleUserText("结束学习"));
}

void TestExitRestoresServer() {
    printf("--- 退出教学 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    CHECK(el.HandleUserText("开始学习英语单词"));
    CHECK(g_rec.suppress_now);
    CHECK(el.HandleUserText("结束学习"));
    CHECK(el.state() == State::kIdle);
    CHECK(!g_rec.suppress_now);
    // 主动退出：播一句收尾提示，然后回到待机（等唤醒词）
    CHECK(g_rec.played.back() == "prompt_exit.ogg");
    g_rec.Clear();
    Drain();
    CHECK(g_rec.idles.size() == 1);
    CHECK(g_rec.idles.back() == "已结束英语学习");
    CHECK(g_rec.listening.empty());
}

void TestQuestionHandsBackToServer() {
    printf("--- 改问其它问题 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    CHECK(el.HandleUserText("开始学习英语单词"));
    CHECK(el.HandleUserText("第三单元"));
    CHECK(el.state() == State::kStudying);

    // 问了个问题 -> 交回服务端：不能进待机，否则服务端的回答会被丢掉
    CHECK(!el.HandleUserText("今天天气怎么样"));
    CHECK(el.state() == State::kIdle);
    CHECK(!g_rec.suppress_now);
    CHECK(g_rec.idles.empty());
    CHECK(g_rec.listening.back());
}

void TestNavigation() {
    printf("--- 词条导航 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    CHECK(el.HandleUserText("开始学习英语单词"));
    CHECK(el.HandleUserText("第三单元"));
    CHECK(el.state() == State::kStudying);
    CHECK(el.current_index() == 0);

    // 「下一个」-> 第 2 个词
    g_rec.Clear();
    CHECK(el.HandleUserText("下一个"));
    CHECK(el.current_index() == 1);
    CHECK(g_rec.cards.size() == 1);
    CHECK(g_rec.cards[0].index == 1);

    // 「上一个」-> 退回第 1 个词
    g_rec.Clear();
    CHECK(el.HandleUserText("上一个"));
    CHECK(el.current_index() == 0);
    CHECK(g_rec.cards.size() == 1);
    CHECK(g_rec.cards[0].index == 0);

    // 「第三个」-> 第 3 个词（index 2）
    g_rec.Clear();
    CHECK(el.HandleUserText("第三个"));
    CHECK(el.current_index() == 2);
    CHECK(g_rec.cards[0].index == 2);

    // 「第5个」-> 第 5 个词（index 4）
    g_rec.Clear();
    CHECK(el.HandleUserText("第5个"));
    CHECK(el.current_index() == 4);
    CHECK(g_rec.cards[0].index == 4);

    // 「第一个」-> 回到开头
    g_rec.Clear();
    CHECK(el.HandleUserText("第一个"));
    CHECK(el.current_index() == 0);

    // 「最后一个」-> 末尾（Unit 3 有 18 个词 -> index 17）
    g_rec.Clear();
    CHECK(el.HandleUserText("最后一个"));
    CHECK(el.current_index() == 17);
    CHECK(g_rec.cards[0].is_last);

    // 越界 -> 提示没听清，且不改变当前位置
    g_rec.Clear();
    CHECK(el.HandleUserText("第99个"));
    CHECK(el.current_index() == 17);
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "prompt_confused.ogg");
}

void TestListUnits() {
    printf("--- 单元列表 ---\n");
    const auto units = EnglishLearning::GetInstance().ListUnits();
    CHECK(units.size() == 6);
    CHECK(units[0].unit == 1);
    CHECK(units[0].word_count == 19);
    CHECK(units[5].unit == 6);
    CHECK(units[5].word_count == 21);
    const std::string text = EnglishLearning::GetInstance().ListUnitsText();
    CHECK(text.find("Unit 1") != std::string::npos);
    CHECK(text.find("Unit 6") != std::string::npos);
}

}  // namespace

int main() {
    TestAudioResolver();
    TestIntentParserStillGreen();
    TestFullScript();
    TestExitRestoresServer();
    TestQuestionHandsBackToServer();
    TestNavigation();
    TestListUnits();

    printf("\n断言 %d 条，失败 %d 条\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
