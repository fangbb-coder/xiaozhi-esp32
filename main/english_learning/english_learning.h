/**
 * @file english_learning.h
 * @brief 门口迎宾之外的第二个扩展应用：英语单词学习
 *
 * 交互剧本（全部本地执行，服务端只负责 ASR）：
 *   用户：「开始学习英语单词」
 *   设备：「好的，开始学习英语单词。请问学习第几单元？」
 *   用户：「第三单元」
 *   设备：显示第 3 单元第 1 个词卡 → 读单词 → 读拼写 → 读中文意思
 *   用户：「下一个」
 *   设备：下一个词……
 *   （本单元读完）设备：「本单元单词学习结束。」
 *
 * 设计要点
 * --------
 * * **设备端自治**：意图识别、词表、发音全在本地，服务端只把 ASR 文本推过来。
 *   发音用预生成的离线音频（编进固件），不依赖服务端 TTS，发音绝对准确。
 * * **不打断原有功能**：只有识别到教学意图时才接管这一轮，其余对话原样走服务端。
 * * **零 RAM 常驻**：词表是编译期字符串字面量（flash），音频是链接符号（flash）。
 *
 * 播报编排
 * --------
 * 一次只往播放队列里推一段音频，靠 ``on_playback_drained`` 回调推进下一段。
 * 这样能精确知道「这一段放完了」，不会误判成两段连在一起。
 */
#ifndef ENGLISH_LEARNING_H_
#define ENGLISH_LEARNING_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace english_learning {

struct WordEntry;  // word_table.h（自动生成）

/** 教学模式的状态 */
enum class State {
    kIdle = 0,        /**< 没在教学 */
    kChoosingUnit,    /**< 已问"第几单元"，等用户回答 */
    kStudying,        /**< 正在逐词学习 */
};

/** 一个单词的完整信息（交给屏幕渲染成一张卡） */
struct StudyCard {
    int unit = 0;           /**< 1-based 单元号 */
    int index = 0;          /**< 0-based，本单元内第几个词 */
    int word_count = 0;     /**< 本单元总词数 */
    const char* word = "";  /**< 英文单词 */
    const char* spell = ""; /**< 逐字母拆解，如 "P-L-A-Y-G-R-O-U-N-D" */
    const char* zh = "";    /**< 汉语意思 */
    bool is_last = false;   /**< 是不是本单元最后一个词 */
};

/** 单元摘要（list_units 用） */
struct UnitSummary {
    int unit = 0;
    const char* title = "";
    int word_count = 0;
};

class EnglishLearning {
public:
    static EnglishLearning& GetInstance();

    /** 需要由 Application 注入的能力 */
    struct Hooks {
        /** 播一个编进固件的音频资源（资源名如 "en_u3_w03.ogg"） */
        std::function<void(const std::string& asset_name)> play_asset;
        /** 显示单词卡 */
        std::function<void(const StudyCard& card)> show_card;
        /** 显示一行提示文字（不播音） */
        std::function<void(const std::string& text)> show_text;
        /** 进入 / 退出聆听（true=开始听，false=停止听，转入播报态） */
        std::function<void(bool start)> set_listening;
        /** 屏蔽 / 恢复服务端播报（本地教学期间服务端不要插话） */
        std::function<void(bool suppress)> suppress_server_speech;
        /**
         * @brief 回到待机状态（等待唤醒词）
         *
         * 设备端实现必须在状态切换生效之后再显示 text，因为待机状态会清空
         * 聊天区，提前显示会被擦掉。
         */
        std::function<void(const std::string& text)> set_idle;
    };

    void SetHooks(Hooks hooks);

    /**
     * @brief 收到 ASR 文本时调用
     * @return true 表示设备接管了这一轮（服务端不要再回答）
     */
    bool HandleUserText(const std::string& text);

    /** 播放队列排空时调用，驱动播报编排前进 */
    void OnPlaybackFinished();

    bool active() const { return state_ != State::kIdle; }
    State state() const { return state_; }
    int current_unit() const { return unit_; }
    int current_index() const { return index_; }

    /** 列出所有单元（MCP 工具用） */
    std::vector<UnitSummary> ListUnits() const;
    std::string ListUnitsText() const;

    /**
     * @brief 退出教学模式
     * @param to_idle true=播一句收尾提示后回到待机状态（等唤醒词）；
                 false=把这一轮交回服务端继续回答
     */
    void Exit(bool to_idle);

private:
    EnglishLearning() = default;

    /** 一段音频播完之后要做什么 */
    enum class AfterPlayback {
        kNone,
        kPromptFinished, /**< 一条提示语播完了 */
        kWordFinished,   /**< 一个词的三段播完了 */
        kUnitFinished,   /**< 单元结束语播完了 */
        kExitFinished,   /**< 退出提示语播完了 */
    };

    void EnqueueAndPlay(std::vector<std::string> items, AfterPlayback after);
    void PlayStep();
    void HandleAfterPlayback();
    void StartChoosingUnit();
    void StartUnit(int unit);
    void PlayCurrentWord();
    void AdvanceStep();
    void AfterWord();
    void FinishUnit();
    void ResumeListening();
    void ShowCard();
    void SetState(State state);
    const WordEntry& CurrentWord() const;
    int CurrentWordCount() const;
    std::string AssetName(int unit, int index, const char* kind) const;

    State state_ = State::kIdle;
    int unit_ = 0;    /**< 1-based */
    int index_ = 0;   /**< 0-based */

    /** 当前正在播的这一串资源 */
    std::vector<std::string> pending_;
    size_t step_ = 0;
    bool awaiting_drain_ = false;

    /** 播完整段之后的动作 */
    AfterPlayback after_ = AfterPlayback::kNone;

    Hooks hooks_;
    bool hooks_ready_ = false;
};

}  // namespace english_learning

#endif  // ENGLISH_LEARNING_H_
