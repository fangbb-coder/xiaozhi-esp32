/**
 * @file english_learning.cc
 * @brief 英语单词学习的设备端状态机
 */

#include "english_learning.h"

#include "english_audio.h"
#include "intent_parser.h"
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
constexpr const char* kKindZh = "zh";
/** 提示语资源名 */
constexpr const char* kPromptStart = "prompt_start.ogg";
constexpr const char* kPromptAskUnit = "prompt_ask_unit.ogg";
constexpr const char* kPromptListUnits = "prompt_list_units.ogg";
constexpr const char* kPromptUnitDone = "prompt_unit_done.ogg";
constexpr const char* kPromptRepeat = "prompt_repeat.ogg";
constexpr const char* kPromptConfused = "prompt_confused.ogg";
constexpr const char* kPromptExit = "prompt_exit.ogg";

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
    static const WordEntry kEmpty = {"", "", "", 0, 0, 0};
    if (unit_ < 1 || unit_ > static_cast<int>(kEnglishUnitCount)) {
        return kEmpty;
    }
    const UnitEntry& unit = kEnglishUnits[unit_ - 1];
    if (index_ < 0 || index_ >= unit.word_count) {
        return kEmpty;
    }
    return unit.words[index_];
}

std::string EnglishLearning::AssetName(int unit, int index, const char* kind) const {
    char buffer[48];
    // 词表里的序号是 1-based，资源名也用 1-based，和音频生成脚本保持一致。
    // 拼读音频是 en_u3_w05_spell.ogg，词/中文是 en_u3_w05.ogg / zh_u3_w05.ogg。
    if (strcmp(kind, kKindSpell) == 0) {
        snprintf(buffer, sizeof(buffer), "en_u%d_w%02d_spell.ogg", unit, index + 1);
    } else {
        snprintf(buffer, sizeof(buffer), "%s_u%d_w%02d.ogg", kind, unit, index + 1);
    }
    return std::string(buffer);
}

bool EnglishLearning::HandleUserText(const std::string& text) {
    if (!hooks_ready_) {
        return false;
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
                AdvanceStep();
                return true;
            }
            if (state_ == State::kChoosingUnit) {
                EnqueueAndPlay({kPromptConfused}, AfterPlayback::kPromptFinished);
                return true;
            }
            return false;

        case Intent::kRepeat:
            if (state_ == State::kStudying) {
                // 「好的，再读一遍」+ 当前词的三段，合成一条播放序列
                std::vector<std::string> items = {kPromptRepeat};
                const WordEntry& word = CurrentWord();
                if (word.en != nullptr && word.en[0] != '\0') {
                    items.push_back(AssetName(unit_, index_, kKindWord));
                    items.push_back(AssetName(unit_, index_, kKindSpell));
                    items.push_back(AssetName(unit_, index_, kKindZh));
                }
                EnqueueAndPlay(std::move(items), AfterPlayback::kWordFinished);
                return true;
            }
            return false;

        case Intent::kExit:
            if (state_ != State::kIdle) {
                Exit();
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
        Exit();
        return false;
    }

    // 没听懂：提示一句，留在教学模式里
    EnqueueAndPlay({kPromptConfused}, AfterPlayback::kPromptFinished);
    return true;
}

void EnglishLearning::StartChoosingUnit() {
    SetState(State::kChoosingUnit);
    unit_ = 0;
    index_ = 0;
    if (hooks_.suppress_server_speech) {
        hooks_.suppress_server_speech(true);
    }
    EnqueueAndPlay({kPromptStart, kPromptAskUnit}, AfterPlayback::kPromptFinished);
}

void EnglishLearning::StartUnit(int unit) {
    SetState(State::kStudying);
    unit_ = unit;
    index_ = 0;
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
    EnqueueAndPlay({AssetName(unit_, index_, kKindWord), AssetName(unit_, index_, kKindSpell),
                     AssetName(unit_, index_, kKindZh)},
                    AfterPlayback::kWordFinished);
}

void EnglishLearning::AdvanceStep() {
    const int count = CurrentWordCount();
    if (count <= 0) {
        return;
    }
    if (index_ + 1 >= count) {
        // 已经是最后一个了，再喊「下一个」就当作本单元结束
        FinishUnit();
        return;
    }
    index_++;
    ShowCard();
    PlayCurrentWord();
}

void EnglishLearning::AfterWord() {
    const int count = CurrentWordCount();
    if (count > 0 && index_ + 1 >= count) {
        FinishUnit();
        return;
    }
    ResumeListening();
}

void EnglishLearning::FinishUnit() {
    ESP_LOGI(TAG, "unit %d finished", unit_);
    SetState(State::kIdle);
    if (hooks_.show_text) {
        hooks_.show_text("本单元单词学习结束");
    }
    EnqueueAndPlay({kPromptUnitDone}, AfterPlayback::kUnitFinished);
}

void EnglishLearning::Exit() {
    ESP_LOGI(TAG, "exit learning mode");
    pending_.clear();
    step_ = 0;
    awaiting_drain_ = false;
    after_ = AfterPlayback::kNone;
    SetState(State::kIdle);
    if (hooks_.suppress_server_speech) {
        hooks_.suppress_server_speech(false);
    }
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

void EnglishLearning::ShowCard() {
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
    card.zh = word.zh;
    card.is_last = (count > 0 && index_ + 1 >= count);
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
        case AfterPlayback::kUnitFinished:
            // 本单元学完，交回普通对话
            if (hooks_.suppress_server_speech) {
                hooks_.suppress_server_speech(false);
            }
            ResumeListening();
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
