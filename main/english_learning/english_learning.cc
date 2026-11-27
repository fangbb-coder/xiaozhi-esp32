/**
 * @file english_learning.cc
 * @brief 英语单词学习的设备端状态机
 *
 * 状态流转
 * --------
 *   kIdle ──「开始学习英语单词」──▶ kChoosingUnit ──「第三单元」──▶ kStudying
 *                                                                      │
 *                            词的三段播完 ◀───────────────────────────┘
 *                                 │
 *                                 ▼
 *                            kPractice ──读对了──▶ kConfirmNext ──「下一个」──▶ kStudying(下一个词)
 *                                 │                        │
 *                                 │ 读错了                 │ 超时
 *                                 ▼                        ▼
 *                            kPracticeRetry          下一个词
 *                                 │
 *              连续 2 次读不出 ────┴──▶ 播正确读法 ──▶ 还有 1 次机会
 *                                 │
 *                       3 次机会用完
 *                                 ▼
 *                            放过这个词 ──▶ kConfirmNext
 *
 *   最后一个词读对（或放过）──▶ kUnitSummary ──鼓掌+鼓励──▶ 回待机
 */

#include "english_learning.h"

#include "english_audio.h"
#include "intent_parser.h"
#include "word_match.h"
#include "word_table.h"

#include <esp_log.h>

#include <cstdio>
#include <cstring>

namespace english_learning {

namespace {

const char* TAG __attribute__((unused)) = "EnglishLearning";

/** 资源名后缀 */
constexpr const char* kKindWord = "en";
constexpr const char* kKindSpell = "en_spell";
constexpr const char* kKindSyllable = "en_syll";
constexpr const char* kKindZh = "zh";

/** 提示语资源名 */
constexpr const char* kPromptStart = "prompt_start.ogg";
constexpr const char* kPromptAskUnit = "prompt_ask_unit.ogg";
constexpr const char* kPromptListUnits = "prompt_list_units.ogg";
constexpr const char* kPromptRepeat = "prompt_repeat.ogg";
constexpr const char* kPromptConfused = "prompt_confused.ogg";
constexpr const char* kPromptExit = "prompt_exit.ogg";

/** 跟读环节的提示语 */
constexpr const char* kPromptPractice = "prompt_practice.ogg";   // 「请跟我读一遍。」
constexpr const char* kPromptTryAgain = "prompt_try_again.ogg";  // 「没关系，再试一次。」
constexpr const char* kPromptCorrect = "prompt_correct.ogg";      // 「听一听正确的读法。」
constexpr const char* kPromptNextQ = "prompt_next_q.ogg";         // 「要继续下一个吗？」
constexpr const char* kPromptGiveUp = "prompt_giveup.ogg";        // 「这个词先放一放，我们继续。」
constexpr const char* kPromptLastWord = "prompt_last_word.ogg";  // 「这是最后一个词啦，加油！」
constexpr const char* kPromptUnitPraise = "prompt_unit_praise.ogg";
constexpr const char* kPromptSummary = "prompt_summary.ogg";
constexpr const char* kApplause = "applause.ogg";

/** 鼓励语轮换表：每次都说同一句太机械 */
constexpr const char* const kPraiseAssets[] = {
    "praise_1.ogg", "praise_2.ogg", "praise_3.ogg", "praise_4.ogg",
};

/** 等孩子开口跟读的超时 */
constexpr int kPracticeTimeoutMs = 15000;
/** 等孩子回答「要不要继续」的超时 */
constexpr int kConfirmTimeoutMs = 20000;
/** 每个词最多给几次机会 */
constexpr int kMaxAttempts = 3;

/**
 * @brief 判断一句话是不是在提问
 *
 * 学习过程中用户如果忽然问了个问题（例如「今天天气怎么样」），应该把对话交回
 * 服务端，而不是当成「没听清」一直卡在教学里。
 */
bool LooksLikeQuestion(const std::string& text) {
    static const char* kMarkers[] = {"什么", "怎么", "为什么", "哪", "吗", "?", "？", "谁", "多少"};
    for (const char* marker : kMarkers) {
        if (text.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

EnglishLearning& EnglishLearning::GetInstance() {
    static EnglishLearning instance;
    return instance;
}

void EnglishLearning::SetHooks(Hooks hooks) {
    hooks_ = std::move(hooks);
    hooks_ready_ = static_cast<bool>(hooks_.play_asset) && static_cast<bool>(hooks_.show_card);
    ESP_LOGI(TAG, "hooks installed (ready=%d)", hooks_ready_ ? 1 : 0);
}

void EnglishLearning::SetState(State state) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    ESP_LOGI(TAG, "state -> %d", static_cast<int>(state_));
}

int EnglishLearning::CurrentWordCount() const {
    if (unit_ < 1 || unit_ > static_cast<int>(kEnglishUnitCount)) {
        return 0;
    }
    return kEnglishUnits[unit_ - 1].word_count;
}

const WordEntry& EnglishLearning::CurrentWord() const {
    static const WordEntry kEmpty = {"", "", "", "", 0, 0, 0, 0};
    if (unit_ < 1 || unit_ > static_cast<int>(kEnglishUnitCount)) {
        return kEmpty;
    }
    const UnitEntry& unit = kEnglishUnits[unit_ - 1];
    if (index_ < 0 || index_ >= unit.word_count) {
        return kEmpty;
    }
    return unit.words[index_];
}

std::string EnglishLearning::CurrentWordText() const {
    const WordEntry& word = CurrentWord();
    return (word.en != nullptr) ? std::string(word.en) : std::string();
}

bool EnglishLearning::IsLastWord() const {
    const int count = CurrentWordCount();
    return count > 0 && index_ + 1 >= count;
}

std::string EnglishLearning::AssetName(int unit, int index, const char* kind) const {
    char buffer[48];
    // 词表里的序号是 1-based，资源名也用 1-based，和音频生成脚本保持一致。
    // 拼读音频是 en_u3_w05_spell.ogg，词/中文是 en_u3_w05.ogg / zh_u3_w05.ogg。
    if (strcmp(kind, kKindSpell) == 0) {
        snprintf(buffer, sizeof(buffer), "en_u%d_w%02d_spell.ogg", unit, index + 1);
    } else if (strcmp(kind, kKindSyllable) == 0) {
        snprintf(buffer, sizeof(buffer), "en_u%d_w%02d_syll.ogg", unit, index + 1);
    } else {
        snprintf(buffer, sizeof(buffer), "%s_u%d_w%02d.ogg", kind, unit, index + 1);
    }
    return std::string(buffer);
}

bool EnglishLearning::HandleUserText(const std::string& text) {
    if (!hooks_ready_) {
        return false;
    }

    // 跟读态和「是否继续」态里，用户说的这句话是「答案」而不是「指令」。
    // 这两个状态单独处理：先跟目标词比对，再看是不是指令。
    if (state_ == State::kPractice || state_ == State::kPracticeRetry) {
        return HandlePracticeText(text);
    }
    if (state_ == State::kConfirmNext) {
        return HandleConfirmText(text);
    }

    ParsedIntent parsed = ParseIntent(text, static_cast<int>(kEnglishUnitCount));
    ESP_LOGI(TAG, "text='%s' intent=%s unit=%d state=%d", text.c_str(), IntentName(parsed.intent),
             parsed.unit, static_cast<int>(state_));

    switch (parsed.intent) {
        case Intent::kStart:
            if (state_ == State::kIdle) {
                StartChoosingUnit();
            } else {
                // 已经在学了，重新问一遍第几单元
                EnqueueAndPlay({kPromptAskUnit}, AfterPlayback::kPromptFinished);
            }
            return true;

        case Intent::kSelectUnit:
            if (parsed.unit >= 1 && parsed.unit <= static_cast<int>(kEnglishUnitCount)) {
                StartUnit(parsed.unit);
            } else {
                // 编号越界：让设备反问，而不是装懂
                EnqueueAndPlay({kPromptConfused}, AfterPlayback::kPromptFinished);
            }
            return true;

        case Intent::kNext:
            if (state_ == State::kStudying) {
                // 学习途中喊「下一个」：跳过当前词，直接进下一个
                AdvanceToNextWord();
                return true;
            }
            if (state_ == State::kChoosingUnit) {
                EnqueueAndPlay({kPromptConfused}, AfterPlayback::kPromptFinished);
                return true;
            }
            return false;

        case Intent::kPrev:
            if (state_ == State::kStudying) {
                if (index_ > 0) {
                    index_--;
                }
                // 已经是第一个词了也不报错，直接重播当前词
                ShowCard();
                PlayCurrentWord();
                return true;
            }
            return false;

        case Intent::kGoto:
            if (state_ == State::kStudying) {
                const int count = CurrentWordCount();
                int target = parsed.word_index;
                if (target == -1) {
                    target = count;  // 「最后一个」
                }
                if (target < 1 || target > count) {
                    // 越界：让设备反问，而不是装懂
                    ESP_LOGW(TAG, "goto word %d out of range (count=%d)", target, count);
                    EnqueueAndPlay({kPromptConfused}, AfterPlayback::kPromptFinished);
                    return true;
                }
                index_ = target - 1;
                ShowCard();
                PlayCurrentWord();
                return true;
            }
            return false;

        case Intent::kRepeat:
            if (state_ == State::kStudying) {
                // 「好的，再读一遍」+ 当前词的三段，合成一条播放序列
                std::vector<std::string> items = {kPromptRepeat};
                const WordEntry& word = CurrentWord();
                if (word.en != nullptr && word.en[0] != '\0') {
                    for (const std::string& name : BuildWordSequence(word)) {
                        items.push_back(name);
                    }
                }
                EnqueueAndPlay(std::move(items), AfterPlayback::kWordFinished);
                return true;
            }
            return false;

        case Intent::kExit:
            if (state_ != State::kIdle) {
                // 用户说「退出」→ 收尾后回到待机，等唤醒词
                Exit(true);
                return true;
            }
            return false;

        case Intent::kListUnits:
            if (state_ != State::kIdle) {
                EnqueueAndPlay({kPromptListUnits}, AfterPlayback::kPromptFinished);
                return true;
            }
            // 空闲状态下交给服务端的 MCP 工具去答
            return false;

        case Intent::kNone:
        default:
            break;
    }

    if (state_ == State::kIdle) {
        return false;
    }

    if (state_ == State::kStudying && LooksLikeQuestion(text)) {
        // 用户改问别的了，把这一轮交回服务端
        ESP_LOGI(TAG, "question detected, handing the turn back to the server");
        // 交回服务端：不能进待机，否则服务端的回答会被丢掉
        Exit(false);
        return false;
    }

    // 没听懂：提示一句，留在教学模式里
    EnqueueAndPlay({kPromptConfused}, AfterPlayback::kPromptFinished);
    return true;
}

/* ---------------- 跟读环节 ---------------- */

bool EnglishLearning::HandlePracticeText(const std::string& text) {
    const std::string target = CurrentWordText();
    ESP_LOGI(TAG, "practice text='%s' target='%s' attempt=%d", text.c_str(), target.c_str(),
             attempt_);

    // 第一步：先跟目标词比对。
    // 顺序不能反：词表里真有一个词叫 "now"，孩子读对了它，
    // 不能被当成别的指令给截胡。
    const MatchResult result = MatchSpoken(target, text);
    if (result == MatchResult::kExact || result == MatchResult::kClose) {
        OnPracticeCorrect();
        return true;
    }

    // 第二步：不是读词，那看看是不是指令。
    ParsedIntent parsed = ParseIntent(text, static_cast<int>(kEnglishUnitCount));
    switch (parsed.intent) {
        case Intent::kNext:
            // 「下一个」：不等鼓励也不问要不要继续，直接进下一个词
            ESP_LOGI(TAG, "skip to next word");
            CancelTimeout();
            AdvanceToNextWord();
            return true;
        case Intent::kRepeat:
            // 「再读一遍」：重播这个词的三段，然后继续跟读
            CancelTimeout();
            attempt_ = 0;
            EnqueueAndPlay(BuildWordSequence(CurrentWord()), AfterPlayback::kWordFinished);
            return true;
        case Intent::kExit:
            Exit(true);
            return true;
        default:
            break;
    }

    // 第三步：改问别的了？把这一轮交回服务端。
    if (LooksLikeQuestion(text)) {
        ESP_LOGI(TAG, "question detected during practice, handing the turn back");
        Exit(false);
        return false;
    }

    // 都不是：算一次没读出来
    RegisterPracticeMiss(text, result == MatchResult::kEmpty ? "empty" : "wrong");
    return true;
}

bool EnglishLearning::HandleConfirmText(const std::string& text) {
    ParsedIntent parsed = ParseIntent(text, static_cast<int>(kEnglishUnitCount));
    ESP_LOGI(TAG, "confirm text='%s' intent=%s", text.c_str(), IntentName(parsed.intent));

    switch (parsed.intent) {
        case Intent::kNext:
            CancelTimeout();
            AdvanceToNextWord();
            return true;
        case Intent::kRepeat:
            // 想再听一遍这个词：重播，然后回到跟读
            CancelTimeout();
            attempt_ = 0;
            EnqueueAndPlay(BuildWordSequence(CurrentWord()), AfterPlayback::kWordFinished);
            return true;
        case Intent::kExit:
            Exit(true);
            return true;
        default:
            break;
    }

    if (LooksLikeQuestion(text)) {
        Exit(false);
        return false;
    }

    // 没听清：再问一遍「要继续下一个吗」
    EnqueueAndPlay({kPromptNextQ}, AfterPlayback::kConfirmNextFinished);
    return true;
}

void EnglishLearning::OnPracticeCorrect() {
    correct_count_++;
    CancelTimeout();
    ESP_LOGI(TAG, "practice correct: %d / %d", correct_count_, CurrentWordCount());

    // 读对了：撤掉音节行的跟读高亮
    ShowCard(false);

    if (IsLastWord()) {
        // 最后一个词：读完直接进总结，不再问「要不要继续」
        SetState(State::kUnitSummary);
        EnqueueAndPlay({NextPraiseAsset()}, AfterPlayback::kLastPraiseFinished);
        return;
    }
    SetState(State::kConfirmNext);
    EnqueueAndPlay({NextPraiseAsset(), kPromptNextQ}, AfterPlayback::kPraiseFinished);
}

void EnglishLearning::RegisterPracticeMiss(const std::string& heard, const char* why) {
    // why 只出现在日志里；主机侧测试的 esp_log.h 是空宏，这里显式消掉未使用告警。
    static_cast<void>(why);
    attempt_++;
    ESP_LOGI(TAG, "practice miss #%d (%s) word='%s' heard='%s'", attempt_, why,
             CurrentWordText().c_str(), heard.c_str());

    if (attempt_ >= kMaxAttempts) {
        // 三次机会用完：放过这个词，绝不把孩子卡在这儿
        EnqueueAndPlay({kPromptGiveUp}, AfterPlayback::kGiveUpFinished);
        return;
    }
    if (attempt_ == 2) {
        // 连续两次读不出来：给一次正确读法，再给最后一次机会。
        // 顺便把纠错卡亮出来：屏幕上能看到「你读到的是 X」。
        if (hooks_.show_feedback) {
            hooks_.show_feedback(heard, CurrentWordText());
        }
        std::vector<std::string> items = {kPromptCorrect, AssetName(unit_, index_, kKindWord)};
        EnqueueAndPlay(std::move(items), AfterPlayback::kCorrectFinished);
        return;
    }
    // 第一次：只请重试，不批评
    EnqueueAndPlay({kPromptTryAgain}, AfterPlayback::kTryAgainFinished);
}

void EnglishLearning::GiveUpCurrentWord() {
    CancelTimeout();
    if (IsLastWord()) {
        AfterLastWordDone();
        return;
    }
    SetState(State::kConfirmNext);
    EnqueueAndPlay({kPromptNextQ}, AfterPlayback::kConfirmNextFinished);
}

void EnglishLearning::AdvanceToNextWord() {
    const int count = CurrentWordCount();
    if (count <= 0) {
        return;
    }
    if (index_ + 1 >= count) {
        // 已经是最后一个词了：直接进单元总结
        AfterLastWordDone();
        return;
    }
    index_++;
    SetState(State::kStudying);
    ShowCard();
    PlayCurrentWord();
}

void EnglishLearning::AfterLastWordDone() {
    if (summary_started_) {
        // 已经总结过一次了：别再鼓掌一次
        return;
    }
    summary_started_ = true;
    CancelTimeout();
    SetState(State::kUnitSummary);
    ESP_LOGI(TAG, "unit %d finished, correct=%d/%d", unit_, correct_count_, CurrentWordCount());
    // 总结卡上屏（只上屏，不念数字 —— 数字是动态的，语音里没有）
    if (hooks_.show_summary) {
        hooks_.show_summary(unit_, correct_count_, CurrentWordCount());
    }
    // 顺序：先报「数一数」（总结卡同时上屏）→ 鼓掌 → 单元鼓励语
    EnqueueAndPlay({kPromptSummary, kApplause, kPromptUnitPraise},
                   AfterPlayback::kUnitSummaryFinished);
}

void EnglishLearning::StartPractice() {
    attempt_ = 0;
    practice_timeout_count_ = 0;
    SetState(State::kPractice);
    // 跟读态：音节行高亮，让孩子照着屏幕上的拆分读整个词
    ShowCard(true);
    EnqueueAndPlay({kPromptPractice}, AfterPlayback::kPracticePromptFinished);
}

void EnglishLearning::ArmTimeout(int ms) {
    if (!hooks_.set_timeout) {
        return;
    }
    hooks_.set_timeout(ms, [this]() {
        // 设备端把回调转到主线程才触发，所以这里可以放心动状态。
        if (state_ == State::kPractice || state_ == State::kPracticeRetry) {
            OnPracticeTimeout();
        } else if (state_ == State::kConfirmNext) {
            OnConfirmTimeout();
        }
    });
}

void EnglishLearning::CancelTimeout() {
    if (hooks_.set_timeout) {
        hooks_.set_timeout(0, nullptr);
    }
}

void EnglishLearning::OnPracticeTimeout() {
    if (state_ != State::kPractice && state_ != State::kPracticeRetry) {
        return;
    }
    practice_timeout_count_++;
    ESP_LOGI(TAG, "practice timeout #%d", practice_timeout_count_);
    // 超时也记一次没读出来，但措辞一样温和
    RegisterPracticeMiss("", "timeout");
}

void EnglishLearning::OnConfirmTimeout() {
    if (state_ != State::kConfirmNext) {
        return;
    }
    ESP_LOGI(TAG, "confirm timeout, moving on");
    AdvanceToNextWord();
}

const char* EnglishLearning::NextPraiseAsset() {
    const int count = static_cast<int>(sizeof(kPraiseAssets) / sizeof(kPraiseAssets[0]));
    const char* name = kPraiseAssets[praise_index_ % count];
    praise_index_ = (praise_index_ + 1) % count;
    return name;
}

/* ---------------- 原有流程 ---------------- */

void EnglishLearning::StartChoosingUnit() {
    SetState(State::kChoosingUnit);
    unit_ = 0;
    index_ = 0;
    attempt_ = 0;
    correct_count_ = 0;
    if (hooks_.suppress_server_speech) {
        hooks_.suppress_server_speech(true);
    }
    EnqueueAndPlay({kPromptStart, kPromptAskUnit}, AfterPlayback::kPromptFinished);
}

void EnglishLearning::StartUnit(int unit) {
    SetState(State::kStudying);
    unit_ = unit;
    index_ = 0;
    attempt_ = 0;
    correct_count_ = 0;
    praise_index_ = 0;
    practice_timeout_count_ = 0;
    summary_started_ = false;
    if (hooks_.suppress_server_speech) {
        hooks_.suppress_server_speech(true);
    }
    ShowCard();
    PlayCurrentWord();
}

void EnglishLearning::PlayCurrentWord() {
    const WordEntry& word = CurrentWord();
    if (word.en == nullptr || word.en[0] == '\0') {
        ESP_LOGW(TAG, "no current word (unit=%d index=%d)", unit_, index_);
        return;
    }
    std::vector<std::string> items;
    if (IsLastWord()) {
        // 最后一个词：先说一句「这是最后一个词啦，加油！」
        items.push_back(kPromptLastWord);
    }
    for (const std::string& name : BuildWordSequence(word)) {
        items.push_back(name);
    }
    EnqueueAndPlay(std::move(items), AfterPlayback::kWordFinished);
}

std::vector<std::string> EnglishLearning::BuildWordSequence(const WordEntry& word) const {
    // 教学顺序：单词 → 字母拼读 → 音节拼读 → 汉语意思。
    // 音节拼读只有多音节词才有（单音节词的 syllable 是空串，跳过这一段）。
    std::vector<std::string> items;
    items.push_back(AssetName(unit_, index_, kKindWord));
    items.push_back(AssetName(unit_, index_, kKindSpell));
    if (word.syllable != nullptr && word.syllable[0] != '\0') {
        items.push_back(AssetName(unit_, index_, kKindSyllable));
    }
    items.push_back(AssetName(unit_, index_, kKindZh));
    return items;
}

void EnglishLearning::AdvanceStep() {
    AdvanceToNextWord();
}

void EnglishLearning::AfterWord() {
    // 词的三段播完了 -> 请孩子跟读
    StartPractice();
}

void EnglishLearning::FinishUnit() {
    AfterLastWordDone();
}

void EnglishLearning::Exit(bool to_idle) {
    ESP_LOGI(TAG, "exit learning mode (to_idle=%d)", to_idle ? 1 : 0);
    CancelTimeout();
    pending_.clear();
    step_ = 0;
    awaiting_drain_ = false;
    after_ = AfterPlayback::kNone;
    attempt_ = 0;
    SetState(State::kIdle);
    if (hooks_.suppress_server_speech) {
        hooks_.suppress_server_speech(false);
    }
    if (to_idle) {
        // 用户主动退出：收掉卡片，播一句收尾提示，然后回到待机等唤醒词
        if (hooks_.show_text) {
            hooks_.show_text("已结束英语学习");
        }
        EnqueueAndPlay({kPromptExit}, AfterPlayback::kExitFinished);
        return;
    }
    // 用户改问别的了：这一轮必须交回服务端，否则对方等不到回答
    if (hooks_.show_text) {
        hooks_.show_text("已结束英语学习");
    }
    ResumeListening();
}

void EnglishLearning::ResumeListening() {
    if (hooks_.set_listening) {
        hooks_.set_listening(true);
    }
}

void EnglishLearning::ShowCard(bool practice) {
    if (!hooks_.show_card) {
        return;
    }
    const WordEntry& word = CurrentWord();
    const int count = CurrentWordCount();
    StudyCard card;
    card.unit = unit_;
    card.index = index_;
    card.word_count = count;
    card.word = word.en;
    card.spell = word.spell;
    card.syllable = word.syllable;
    card.zh = word.zh;
    card.is_last = (count > 0 && index_ + 1 >= count);
    card.practice = practice;
    hooks_.show_card(card);
}

void EnglishLearning::EnqueueAndPlay(std::vector<std::string> items, AfterPlayback after) {
    if (items.empty()) {
        return;
    }
    pending_ = std::move(items);
    step_ = 0;
    after_ = after;
    awaiting_drain_ = false;
    PlayStep();
}

void EnglishLearning::PlayStep() {
    if (step_ >= pending_.size()) {
        HandleAfterPlayback();
        return;
    }
    const std::string& name = pending_[step_];
    step_++;
    awaiting_drain_ = true;
    ESP_LOGI(TAG, "play %s (step %d/%d)", name.c_str(), static_cast<int>(step_),
             static_cast<int>(pending_.size()));
    if (hooks_.play_asset) {
        hooks_.play_asset(name);
    }
}

void EnglishLearning::OnPlaybackFinished() {
    if (!awaiting_drain_) {
        // 不是我们的音频放完了（可能是提示音、服务端音频等），忽略
        return;
    }
    awaiting_drain_ = false;
    if (step_ < pending_.size()) {
        PlayStep();
        return;
    }
    HandleAfterPlayback();
}

void EnglishLearning::HandleAfterPlayback() {
    AfterPlayback after = after_;
    after_ = AfterPlayback::kNone;
    pending_.clear();
    step_ = 0;

    switch (after) {
        case AfterPlayback::kPromptFinished:
            ResumeListening();
            break;
        case AfterPlayback::kWordFinished:
            AfterWord();
            break;
        case AfterPlayback::kPracticePromptFinished:
        case AfterPlayback::kTryAgainFinished:
        case AfterPlayback::kCorrectFinished:
            // 「请跟我读一遍」/「再试一次」/「听正确的读法」播完了：等孩子开口
            ResumeListening();
            ArmTimeout(kPracticeTimeoutMs);
            break;
        case AfterPlayback::kPraiseFinished:
        case AfterPlayback::kConfirmNextFinished:
            // 等孩子回答「要不要继续下一个」
            ResumeListening();
            ArmTimeout(kConfirmTimeoutMs);
            break;
        case AfterPlayback::kLastPraiseFinished:
            AfterLastWordDone();
            break;
        case AfterPlayback::kGiveUpFinished:
            GiveUpCurrentWord();
            break;
        case AfterPlayback::kUnitSummaryFinished:
            // 总结完了：回到待机状态，等用户说唤醒词再开始下一轮
            SetState(State::kIdle);
            if (hooks_.suppress_server_speech) {
                hooks_.suppress_server_speech(false);
            }
            if (hooks_.set_idle) {
                hooks_.set_idle("本单元单词学习结束");
            }
            break;
        case AfterPlayback::kUnitFinished:
            if (hooks_.suppress_server_speech) {
                hooks_.suppress_server_speech(false);
            }
            if (hooks_.set_idle) {
                hooks_.set_idle("本单元单词学习结束");
            }
            break;
        case AfterPlayback::kExitFinished:
            // 用户主动退出：同样回到待机状态
            if (hooks_.set_idle) {
                hooks_.set_idle("已结束英语学习");
            }
            break;
        case AfterPlayback::kNone:
        default:
            break;
    }
}

std::vector<UnitSummary> EnglishLearning::ListUnits() const {
    std::vector<UnitSummary> units;
    units.reserve(kEnglishUnitCount);
    for (int i = 0; i < static_cast<int>(kEnglishUnitCount); i++) {
        UnitSummary summary;
        summary.unit = i + 1;
        summary.title = kEnglishUnits[i].title;
        summary.word_count = kEnglishUnits[i].word_count;
        units.push_back(summary);
    }
    return units;
}

std::string EnglishLearning::ListUnitsText() const {
    std::string text = "英语单词学习共 6 个单元：";
    for (int i = 0; i < static_cast<int>(kEnglishUnitCount); i++) {
        text += "\n";
        text += kEnglishUnits[i].title;
        text += "（";
        text += std::to_string(kEnglishUnits[i].word_count);
        text += " 个单词）";
    }
    text += "\n可以说「开始学习英语单词」来开始。";
    return text;
}

}  // namespace english_learning
