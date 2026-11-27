/**
 * @file english_audio.h
 * @brief 取用编进固件的英语学习音频
 *
 * 音频用 ESP-IDF 的 ``EMBED_FILES`` 编进 app 分区，链接符号由
 * ``english_audio_table.h``（自动生成）声明。
 *
 * 之所以不直接用符号名，是因为 IDF 对嵌入文件生成的符号名取决于路径取法，
 * 不同版本/调用方式不一致。这里对三种取法都声明弱符号，运行时挑能用的那个。
 */
#ifndef ENGLISH_LEARNING_ENGLISH_AUDIO_H_
#define ENGLISH_LEARNING_ENGLISH_AUDIO_H_

#include <cstddef>
#include <string>

namespace english_learning {

/** 一段编进固件的 ogg 音频（原始字节，可直接喂给 AudioService::PlaySound） */
struct EnglishAudio {
    const char* data = nullptr;
    size_t size = 0;
};

/**
 * @brief 按资源名取一段编进固件的音频
 *
 * @param name 资源名，如 "en_u3_w05.ogg"
 * @param out  命中时写入数据指针和长度
 * @return 命中返回 true
 */
bool GetEnglishAudio(const std::string& name, EnglishAudio* out);

/**
 * @brief 启动自检：能用的候选链接符号序号
 * @return 0..2 表示命中的候选；-1 表示一个都没命中（音频表没被编进固件）
 */
int EnglishAudioCandidateIndex();

/** 启动时打一行自检日志，顺带统计有多少音频可用 */
void LogEnglishAudioSummary();

}  // namespace english_learning

#endif  // ENGLISH_LEARNING_ENGLISH_AUDIO_H_
