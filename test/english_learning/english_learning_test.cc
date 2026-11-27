/**
 * @file english_learning_test.cc
 * @brief 英语学习模块的主机侧单元测试
 *
 * 覆盖三件事：
 *  1) 音频查表的「多候选弱符号」回退机制（候选 0 / 候选 1 各命中一次）
 *  2) 教学状态机的完整剧本：开始 → 选单元 → 逐词 → 跟读 → 单元总结
 *  3) 跟读环节的判定与兜底：读错重试、连续错纠正、三次机会放过、超时兜底
 *
 * 超时用假的定时器：hooks.set_timeout 把回调存下来，测试自己决定什么时候触发。
 * 编译见 tools/host-syntax 下的说明。
 */

#include <cstdio>
#include <functional>
#include <string>
#include <utility>
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
    std::vector<std::pair<std::string, std::string>> feedbacks;  // heard, expected
    std::vector<std::pair<int, int>> summaries;                 // correct, total
    int summary_unit = 0;
    bool suppress_now = false;
    int timeout_ms = 0;
    std::function<void()> timeout_cb;

    void Clear() {
        played.clear();
        cards.clear();
        texts.clear();
        listening.clear();
        suppress.clear();
        idles.clear();
        feedbacks.clear();
        summaries.clear();
        // 注意：不要在这里清 timeout_cb —— 有些用例要先 Clear() 再触发超时。
    }
};

Recorder g_rec;

void Drain();

/** 安全地看最后一次播了什么（空数组时返回 false，而不是越界崩掉） */
bool LastPlayed(const char* name) {
    return !g_rec.played.empty() && g_rec.played.back() == name;
}

/** 安全地看最后是不是在聆听 */
bool LastListening() {
    return !g_rec.listening.empty() && g_rec.listening.back();
}

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
    hooks.set_timeout = [](int ms, std::function<void()> callback) {
        // ms <= 0 = 取消定时器
        g_rec.timeout_ms = ms;
        g_rec.timeout_cb = std::move(callback);
    };
    hooks.show_feedback = [](const std::string& heard, const std::string& expected) {
        g_rec.feedbacks.emplace_back(heard, expected);
    };
    hooks.show_summary = [](int unit, int correct, int total) {
        g_rec.summaries.emplace_back(correct, total);
        g_rec.summary_unit = unit;
    };
    EnglishLearning::GetInstance().SetHooks(std::move(hooks));
    // 复位：上一个测试可能停在跟读态/确认态，不复位状态会串到下一个测试。
    EnglishLearning::GetInstance().Exit(true);
    Drain();
    g_rec.timeout_cb = nullptr;
    g_rec.timeout_ms = 0;
    g_rec.Clear();
}

/** 模拟一次「播放队列排空」 */
void Drain() { EnglishLearning::GetInstance().OnPlaybackFinished(); }

/** 触发假定时器 */
void FireTimeout() {
    if (g_rec.timeout_cb) {
        auto callback = g_rec.timeout_cb;
        g_rec.timeout_cb = nullptr;
        callback();
    }
}

/** 播到一个词的各段结束、进入「等孩子跟读」为止 */
void DrainToPractice(EnglishLearning& el) {
    for (int i = 0; i < 12; i++) {
        Drain();
        if (el.state() == State::kPractice) {
            break;
        }
    }
    // 「请跟我读一遍」这一段也要放完，之后才是真的在听
    Drain();
}

/** 走到「第三单元第一个词在等跟读」这个状态 */
void EnterPracticeAtFirstWord(EnglishLearning& el) {
    CHECK(el.HandleUserText("开始学习英语单词"));
    Drain();  // prompt_ask_unit
    Drain();  // -> 聆听
    CHECK(el.HandleUserText("第三单元"));
    CHECK(el.state() == State::kStudying);
    DrainToPractice(el);
    CHECK(el.state() == State::kPractice);
}

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
    CHECK(ParseIntent("继续", 6).intent == Intent::kNext);
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
    CHECK(ParseIntent("停止", 6).intent == Intent::kExit);
    CHECK(ParseIntent("再读一遍", 6).intent == Intent::kRepeat);
    CHECK(ParseIntent("有哪些单元", 6).intent == Intent::kListUnits);
    CHECK(ParseIntent("今天天气怎么样", 6).intent == Intent::kNone);
}

void TestPracticeHappyPath() {
    printf("--- 跟读：一次读对 ---\n");
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
    CHECK(LastListening());

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
    CHECK(!g_rec.cards[0].practice);
    // 播报顺序：词 -> 拼写 -> 音节 -> 中文（afternoon 是多音节词，多一段）
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "en_u3_w01.ogg");
    g_rec.played.clear();
    Drain();
    CHECK(g_rec.played[0] == "en_u3_w01_spell.ogg");
    g_rec.played.clear();
    Drain();
    CHECK(g_rec.played[0] == "en_u3_w01_syll.ogg");
    g_rec.played.clear();
    Drain();
    CHECK(g_rec.played[0] == "zh_u3_w01.ogg");

    // 4b) 全部放完 -> 进入跟读态，音节行要高亮
    g_rec.Clear();
    Drain();
    CHECK(el.state() == State::kPractice);
    CHECK(g_rec.cards.back().practice);
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "prompt_practice.ogg");

    // 4c) 「请跟我读一遍」放完 -> 开始听，并装上跟读超时
    g_rec.Clear();
    Drain();
    CHECK(g_rec.played.empty());
    CHECK(LastListening());
    CHECK(g_rec.timeout_ms == 15000);

    // 5) 孩子读对了
    g_rec.Clear();
    CHECK(el.HandleUserText("afternoon"));
    CHECK(el.correct_count() == 1);
    CHECK(el.state() == State::kConfirmNext);
    // 高亮撤掉
    CHECK(!g_rec.cards.back().practice);
    // 鼓励语 + 问要不要继续
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "praise_1.ogg");
    g_rec.Clear();
    Drain();
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "prompt_next_q.ogg");
    g_rec.Clear();
    Drain();
    CHECK(g_rec.played.empty());
    // 等「要不要继续」的超时
    CHECK(g_rec.timeout_ms == 20000);

    // 6) 「下一个」
    g_rec.Clear();
    CHECK(el.HandleUserText("下一个"));
    CHECK(el.current_index() == 1);
    CHECK(g_rec.cards.size() == 1);
    CHECK(g_rec.cards[0].index == 1);
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "en_u3_w02.ogg");

    // 7) 第二个词只有三段（there 是单音节词）
    Drain();
    CHECK(LastPlayed("en_u3_w02_spell.ogg"));
    Drain();
    CHECK(LastPlayed("zh_u3_w02.ogg"));
    Drain();
    CHECK(el.state() == State::kPractice);
    CHECK(LastPlayed("prompt_practice.ogg"));

    // 8) 鼓励语是轮换的：这一次应该是 praise_2
    g_rec.Clear();
    CHECK(el.HandleUserText("there"));
    CHECK(el.correct_count() == 2);
    CHECK(g_rec.played[0] == "praise_2.ogg");
}

void TestPracticeRetryAndGiveUp() {
    printf("--- 跟读：读错重试 / 连续错纠正 / 三次机会放过 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    EnterPracticeAtFirstWord(el);

    // 第一次读错：只请重试，不亮纠错卡
    g_rec.Clear();
    CHECK(el.HandleUserText("elephant"));
    CHECK(el.attempt() == 1);
    CHECK(el.correct_count() == 0);
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "prompt_try_again.ogg");
    CHECK(g_rec.feedbacks.empty());
    Drain();
    CHECK(LastListening());

    // 第二次读错：给正确读法，并亮出纠错卡
    g_rec.Clear();
    CHECK(el.HandleUserText("qqqq"));
    CHECK(el.attempt() == 2);
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "prompt_correct.ogg");
    CHECK(g_rec.feedbacks.size() == 1);
    CHECK(g_rec.feedbacks[0].first == "qqqq");
    CHECK(g_rec.feedbacks[0].second == "afternoon");
    Drain();
    CHECK(LastPlayed("en_u3_w01.ogg"));
    Drain();
    CHECK(LastListening());

    // 第三次读错：放过这个词，别把孩子卡住
    g_rec.Clear();
    CHECK(el.HandleUserText("wwww"));
    CHECK(el.attempt() == 3);
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "prompt_giveup.ogg");
    Drain();
    CHECK(el.state() == State::kConfirmNext);
    CHECK(LastPlayed("prompt_next_q.ogg"));
    Drain();
    CHECK(LastListening());

    // 放过这个词不算读对
    CHECK(el.correct_count() == 0);
}

void TestPracticeAnswerAfterRetry() {
    printf("--- 跟读：重试之后读对 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    EnterPracticeAtFirstWord(el);

    CHECK(el.HandleUserText("zzzzz"));
    CHECK(el.attempt() == 1);
    Drain();
    CHECK(el.HandleUserText("qqqq"));
    CHECK(el.attempt() == 2);
    Drain();
    Drain();
    // 第三次机会读对了：算读对（attempt 记的是失败次数，读对不增加）
    g_rec.Clear();
    CHECK(el.HandleUserText("Afternoon!"));
    CHECK(el.attempt() == 2);
    CHECK(el.correct_count() == 1);
    CHECK(el.state() == State::kConfirmNext);
    CHECK(g_rec.played[0] == "praise_1.ogg");
}

void TestPracticeEmptyIsNotWrong() {
    printf("--- 跟读：没听清不算读错 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    EnterPracticeAtFirstWord(el);

    // 孩子说了一句中文（「我不会说」），归一化后是空串 -> kEmpty
    g_rec.Clear();
    CHECK(el.HandleUserText("我不会说"));
    CHECK(el.attempt() == 1);
    CHECK(el.correct_count() == 0);
    // 走的是「再试一次」，不是「给正确读法」
    CHECK(g_rec.played[0] == "prompt_try_again.ogg");
}

void TestPracticeNextSkipsDirectly() {
    printf("--- 跟读中说「下一个」直接进下一个词 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    EnterPracticeAtFirstWord(el);

    g_rec.Clear();
    CHECK(el.HandleUserText("下一个"));
    // 不问「要不要继续」，也不给鼓励语，直接进下一个词
    CHECK(el.current_index() == 1);
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "en_u3_w02.ogg");
    CHECK(el.correct_count() == 0);
    CHECK(el.state() == State::kStudying);
}

void TestPracticeRepeat() {
    printf("--- 跟读中说「再读一遍」重播当前词 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    EnterPracticeAtFirstWord(el);

    g_rec.Clear();
    CHECK(el.HandleUserText("再读一遍"));
    // 重播这个词的三段，然后回到跟读态
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "en_u3_w01.ogg");
    DrainToPractice(el);
    CHECK(el.state() == State::kPractice);
    // 重播不算一次失败
    CHECK(el.attempt() == 0);
}

void TestPracticeTimeout() {
    printf("--- 跟读超时兜底 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    EnterPracticeAtFirstWord(el);
    CHECK(g_rec.timeout_ms == 15000);

    // 孩子一直不说话 -> 超时算一次没读出来
    g_rec.Clear();
    FireTimeout();
    CHECK(el.attempt() == 1);
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "prompt_try_again.ogg");
}

void TestConfirmTimeout() {
    printf("--- 「要不要继续」超时兜底 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    EnterPracticeAtFirstWord(el);
    CHECK(el.HandleUserText("afternoon"));
    Drain();
    Drain();
    CHECK(el.state() == State::kConfirmNext);
    CHECK(g_rec.timeout_ms == 20000);

    // 一直不回答 -> 默认进下一个词
    g_rec.Clear();
    FireTimeout();
    CHECK(el.current_index() == 1);
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "en_u3_w02.ogg");
}

void TestUnitSummary() {
    printf("--- 单元总结：鼓掌 + 统计 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    CHECK(el.HandleUserText("开始学习英语单词"));
    Drain();
    Drain();
    CHECK(el.HandleUserText("第三单元"));

    // 跳到最后一个词
    g_rec.Clear();
    CHECK(el.HandleUserText("最后一个"));
    CHECK(el.current_index() == 17);
    CHECK(g_rec.cards.size() == 1);
    CHECK(g_rec.cards[0].is_last);
    // 最后一个词前面先说一句「这是最后一个词啦」
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "prompt_last_word.ogg");
    DrainToPractice(el);
    CHECK(el.state() == State::kPractice);

    // 读对最后一个词 -> 直接进总结，不再问「要不要继续」
    g_rec.Clear();
    CHECK(el.HandleUserText("buy"));
    CHECK(el.correct_count() == 1);
    CHECK(el.state() == State::kUnitSummary);
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "praise_1.ogg");

    // 鼓励语放完 -> 总结卡上屏 + 「数一数」
    g_rec.Clear();
    Drain();
    CHECK(g_rec.summaries.size() == 1);
    CHECK(g_rec.summaries[0].first == 1);
    CHECK(g_rec.summaries[0].second == 18);
    CHECK(g_rec.summary_unit == 3);
    CHECK(g_rec.played.size() == 1);
    CHECK(g_rec.played[0] == "prompt_summary.ogg");
    g_rec.Clear();
    Drain();
    CHECK(g_rec.played[0] == "applause.ogg");
    g_rec.Clear();
    Drain();
    CHECK(g_rec.played[0] == "prompt_unit_praise.ogg");

    // 总结放完 -> 回待机
    g_rec.Clear();
    Drain();
    CHECK(g_rec.played.empty());
    CHECK(g_rec.idles.size() == 1);
    CHECK(g_rec.idles.back() == "本单元单词学习结束");
    CHECK(!g_rec.suppress_now);
    CHECK(el.state() == State::kIdle);

    // 待机态不应该继续聆听，要等用户说唤醒词
    CHECK(g_rec.listening.empty());
}

void TestExitDuringPractice() {
    printf("--- 跟读中退出 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    EnterPracticeAtFirstWord(el);
    CHECK(g_rec.suppress_now);

    CHECK(el.HandleUserText("退出"));
    CHECK(el.state() == State::kIdle);
    CHECK(!g_rec.suppress_now);
    CHECK(LastPlayed("prompt_exit.ogg"));
    g_rec.Clear();
    Drain();
    CHECK(g_rec.idles.size() == 1);
    CHECK(g_rec.idles.back() == "已结束英语学习");
}

void TestQuestionDuringPractice() {
    printf("--- 跟读中改问别的 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    EnterPracticeAtFirstWord(el);

    // 问了个问题 -> 交回服务端：不能进待机，否则服务端的回答会被丢掉
    CHECK(!el.HandleUserText("今天天气怎么样"));
    CHECK(el.state() == State::kIdle);
    CHECK(!g_rec.suppress_now);
    CHECK(g_rec.idles.empty());
    CHECK(LastListening());
}

void TestNavigation() {
    printf("--- 词条导航 ---\n");
    InstallHooks();
    EnglishLearning& el = EnglishLearning::GetInstance();
    g_rec.Clear();

    CHECK(el.HandleUserText("开始学习英语单词"));
    Drain();
    Drain();
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
    // 崩溃时别把已打印的内容一起丢掉（stdout 默认是块缓冲）
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestAudioResolver();
    TestIntentParserStillGreen();
    TestPracticeHappyPath();
    TestPracticeRetryAndGiveUp();
    TestPracticeAnswerAfterRetry();
    TestPracticeEmptyIsNotWrong();
    TestPracticeNextSkipsDirectly();
    TestPracticeRepeat();
    TestPracticeTimeout();
    TestConfirmTimeout();
    TestUnitSummary();
    TestExitDuringPractice();
    TestQuestionDuringPractice();
    TestNavigation();
    TestListUnits();

    printf("\n断言 %d 条，失败 %d 条\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
