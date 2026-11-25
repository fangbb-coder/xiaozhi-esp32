/**
 * @file english_audio.cc
 * @brief 英语学习音频的查表与自检
 */

#include "english_audio.h"

#include "english_audio_table.h"

#include <esp_log.h>

#include <cstring>

namespace english_learning {

namespace {

const char* TAG __attribute__((unused)) = "EnglishAudio";

constexpr size_t kCandidateCount = sizeof(kEnglishAudioAssets[0].start) /
                                   sizeof(kEnglishAudioAssets[0].start[0]);

}  // namespace

bool GetEnglishAudio(const std::string& name, EnglishAudio* out) {
    if (out == nullptr) {
        return false;
    }
    for (size_t i = 0; i < kEnglishAudioCount; i++) {
        const EnglishAudioAsset& asset = kEnglishAudioAssets[i];
        if (name != asset.name) {
            continue;
        }
        for (size_t c = 0; c < kCandidateCount; c++) {
            const char* start = asset.start[c];
            const char* end = asset.end[c];
            // 弱符号没定义时链接器给 0，必须先判空再算长度
            if (start == nullptr || end == nullptr || end <= start) {
                continue;
            }
            out->data = start;
            out->size = static_cast<size_t>(end - start);
            return true;
        }
        return false;
    }
    return false;
}

int EnglishAudioCandidateIndex() {
    for (size_t i = 0; i < kEnglishAudioCount; i++) {
        const EnglishAudioAsset& asset = kEnglishAudioAssets[i];
        for (size_t c = 0; c < kCandidateCount; c++) {
            if (asset.start[c] != nullptr && asset.end[c] != nullptr &&
                asset.end[c] > asset.start[c]) {
                return static_cast<int>(c);
            }
        }
    }
    return -1;
}

void LogEnglishAudioSummary() {
    int resolved = 0;
    size_t total = 0;
    for (size_t i = 0; i < kEnglishAudioCount; i++) {
        EnglishAudio audio;
        if (GetEnglishAudio(kEnglishAudioAssets[i].name, &audio)) {
            resolved++;
            total += audio.size;
        }
    }
    const int candidate = EnglishAudioCandidateIndex();
    (void)candidate;
    (void)total;
    if (resolved == static_cast<int>(kEnglishAudioCount)) {
        ESP_LOGI(TAG, "%d/%d audio assets resolved via candidate %d, %.1f KB total",
                 resolved, static_cast<int>(kEnglishAudioCount), candidate, total / 1024.0);
    } else {
        ESP_LOGE(TAG, "only %d/%d audio assets resolved (candidate %d) - the english audio "
                      "files were not embedded into the firmware",
                 resolved, static_cast<int>(kEnglishAudioCount), candidate);
    }
}

}  // namespace english_learning
