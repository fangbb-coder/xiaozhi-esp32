/**
 * @file english_learning.h
 * @brief 门口迎宾之外的第二个扩展应用：英语单词学习
 *
 * 交互剧本（全部本地执行，服务端只负责 ASR）：
 *   用户：「开始学习英语单词」
 *   设备：「好的，开始学习英语单词。请问学习第几单元？」
 *   用户：「第三单元」
 *   设备：显示第 3 单元第 1 个词卡 → 读单词 → 读拼读 → 读音节 → 读中文意思
 *   设备：「请跟我读一遍」→ 屏幕高亮音节行（af-ter-noon）
 *   用户：「afternoon」
 *   设备：「读得真好！」「要继续下一个吗？」
 *   用户：「下一个」
 *   设备：下一个词……（读错时先请重试，连续读不出才纠正）
 *   （本单元读完）设备：鼓掌 → 总结「读对了 N 个」→ 鼓励
 *
 * 设计要点
 * --------
 * * **设备端自治**：意图识别、词表、发音全在本地，服务端只把 ASR 文本推过来。
 *   发音用预生成的离线音频（编进固件），不依赖服务端 TTS，发音绝对准确。
 * * **不打断原有功能**：只有识别到教学意图时才接管这一轮，其余对话原样走服务端。
 * * **零 RAM 常驻**：词表是编译期字符串字面量（flash），音频是链接符号（flash）。
 * * **不打击孩子**：跟读判定刻意宽松（见 word_match.h），
 *   空输入只当「没听清」，第一次读错只请重试，连续读不出才纠正，每词最多 3 次机会，
 *   三次机会用完就放过这个词，流程绝不卡住。
 * * **动态内容不上语音**：孩子读到的词、读对的个数只显示在屏幕上，
 *   语音永远是说「请跟我读一遍」「读得真好」这类提前烘好的固定文案。
 *
 * 播报编排
 * --------
 * 一次只往播放队列里推一段音频，靠 on_playback_drained 回调推进下一段。
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
    kStudying,        /**< 正在播一个词的各段 */
    kPractice,        /**< 播完了，请孩子第一次跟读 */
    kPracticeRetry,   /**< 读错了，请重试，等孩子再读一次 */
    kConfirmNext,     /**< 读完了，问孩子要不要继续下一个 */
    kUnitSummary,     /**< 本单元学完，正在播总结 */
};

/** 单元摘要（list_units 用） */
struct UnitSummary {
    int unit = 0;
    const char* title = "";
    int word_count = 0;
};

/** 一个单词的完整信息（交给屏幕渲染成一张卡） */
struct StudyCard {
    int unit = 0;           /**< 1-based 单元号 */
    int index = 0;          /**< 0-based，本单元内第几个词 */
    int word_count = 0;     /**< 本单元总词数 */
    const char* word = "";  /**< 英文单词 */
    const char* spell = ""; /**< 逐字母拆解，如 "P-L-A-Y-G-R-O-U-N-D" */
    const char* syllable = ""; /**< 音节拆分，如 "af-ter-noon"；单音节词为空串 */
    const char* zh = "";    /**< 汉语意思 */
    bool is_last = false;   /**< 是不是本单元最后一个词 */
    bool practice = false;  /**< 正在等孩子跟读：音节行要高亮 */
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
        /**
         * @brief 设置超时定时器（毫秒）
         *
         * 每次调用会**取消上一个还没触发的定时器**并换成新的。
         * 传 ms <= 0 或空回调 = 取消定时器。
         * 超时是兜底用的：孩子一直不说话时不能把流程卡死。
         * 设备端用 esp_timer 实现（回调转到主线程）；主机侧测试注入假定时器。
         */
        std::function<void(int ms, std::function<void()> callback)> set_timeout;
        /**
         * @brief 显示跟读纠错卡
         *
         * @param heard    孩子读到的内容（ASR 识别出来的文本）
         * @param expected 正确的词
         */
        std::function<void(const std::string& heard, const std::string& expected)> show_feedback;
        /**
         * @brief 显示单元总结
         *
         * @param unit    本单元的编号（1-based）
         * @param correct 本单元读对的词数
         * @param total   本单元总词数
         */
        std::function<void(int unit, int correct, int total)> show_summary;
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
    /** 本单元目前读对的词数 */
    int correct_count() const { return correct_count_; }
    /** 当前词已经失败了几次（0 = 还没读错过） */
    int attempt() const { return attempt_; }

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
        kPromptFinished,       /**< 一条提示语播完了 */
        kWordFinished,         /**< 一个词的各段播完了 → 开始跟读 */
        kPracticePromptFinished, /**< 「请跟我读一遍」播完了 → 等孩子读 */
        kTryAgainFinished,     /**< 「没关系，再试一次」播完了 → 等孩子再读 */
        kCorrectFinished,      /**< 「听一听正确的读法」+ 正确音频播完了 → 还有一次机会 */
        kPraiseFinished,       /**< 鼓励语 + 「要继续吗」播完了 → 等孩子答 */
        kLastPraiseFinished,   /**< 最后一个词的鼓励语播完了 → 本单元总结 */
        kGiveUpFinished,       /**< 「这个词先放一放」播完了 → 问要不要继续 */
        kConfirmNextFinished,  /**< 「要继续下一个吗」播完了 → 等孩子答 */
        kUnitSummaryFinished,  /**< 鼓掌 + 单元鼓励语播完了 */
        kUnitFinished,         /**< 单元结束语播完了 */
        kExitFinished,         /**< 退出提示语播完了 */
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
    void ShowCard(bool practice = false);
    void SetState(State state);
    const WordEntry& CurrentWord() const;
    int CurrentWordCount() const;
    std::string AssetName(int unit, int index, const char* kind) const;
    /** 教学播报顺序：单词 → 字母拼读 → 音节拼读 → 汉语意思 */
    std::vector<std::string> BuildWordSequence(const WordEntry& word) const;

    // ---- 跟读环节 ----
    /** 播完一个词后：请孩子跟读 */
    void StartPractice();
    /**
     * @brief 跟读环节收到文本
     * @return true 表示设备接管了这一轮
     */
    bool HandlePracticeText(const std::string& text);
    /**
     * @brief 「是否继续」环节收到文本
     * @return true 表示设备接管了这一轮
     */
    bool HandleConfirmText(const std::string& text);
    /** 读对了：记分、撤掉高亮、进入「要不要继续」 */
    void OnPracticeCorrect();
    /** 记一次「没读对」（读错或超时），决定下一步怎么引导 */
    void RegisterPracticeMiss(const std::string& heard, const char* why);
    /** 放弃当前词（重试用尽或超时），进入「是否继续」 */
    void GiveUpCurrentWord();
    /** 进入下一个词 */
    void AdvanceToNextWord();
    /** 最后一个词处理完：进单元总结 */
    void AfterLastWordDone();
    /** 装一个超时定时器 */
    void ArmTimeout(int ms);
    /** 取消超时定时器 */
    void CancelTimeout();
    /** 跟读环节超时 */
    void OnPracticeTimeout();
    /** 「是否继续」环节超时：默认进下一个词 */
    void OnConfirmTimeout();
    /** 换一个鼓励语（每次都说同一句太机械） */
    const char* NextPraiseAsset();
    /** 当前词是不是本单元最后一个 */
    bool IsLastWord() const;
    /** 当前词的目标文本（跟读比对的基准） */
    std::string CurrentWordText() const;

    State state_ = State::kIdle;
    int unit_ = 0;    /**< 1-based */
    int index_ = 0;   /**< 0-based */

    /** 当前正在播的这一串资源 */
    std::vector<std::string> pending_;
    size_t step_ = 0;
    bool awaiting_drain_ = false;

    /** 播完整段之后的动作 */
    AfterPlayback after_ = AfterPlayback::kNone;

    /** 当前词已经失败了几次（0 = 还没读错过；每词最多 3 次） */
    int attempt_ = 0;
    /** 本单元读对的词数 */
    int correct_count_ = 0;
    /** 鼓励语的轮换下标 */
    int praise_index_ = 0;
    /** 跟读环节里超时了几次 */
    int practice_timeout_count_ = 0;
    /** 本单元的总结是不是已经做过了（防止重复鼓掌） */
    bool summary_started_ = false;

    Hooks hooks_;
    bool hooks_ready_ = false;
};

}  // namespace english_learning

#endif  // ENGLISH_LEARNING_H_
