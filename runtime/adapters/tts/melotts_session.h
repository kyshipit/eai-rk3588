/*
 * adapters/tts/melotts_session.h — MeloTTS RKNN 合成会话（encoder/decoder + 词表）。
 */
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lexicon.hpp"
#include "melotts_engine.h"

// MeloTTS 模型与词表路径及推理参数（由 main 从 yaml 填充）。
struct MeloTtsConfig {
    std::string encoder_path;
    std::string decoder_path;
    std::string lexicon_path;
    std::string tokens_path;
    std::string language = "ZH_MIX_EN";
    int speak_id = 1;
    float speed = 1.0f;
    bool disable_bert = true;
    int split_min_chars = 8;
    // 不超过此字数时整段一次 decoder，不跑 split_sentence，避免短句首段听不清。
    int single_shot_max_chars = 96;
};

class MeloTtsSession {
public:
    // 逐片段 PCM 回调；返回 false 表示调用方要求停止后续合成。
    using PcmChunkCallback = std::function<bool(std::vector<float>&&)>;

    // 空会话，尚未加载 RKNN。
    MeloTtsSession();
    // 析构时 Shutdown 释放资源。
    ~MeloTtsSession();

    // 加载 RKNN 与词表；可重复调用，失败返回 false。
    bool Init(const MeloTtsConfig& cfg);
    // 释放 RKNN 与词表。
    void Shutdown();
    // 是否已成功 Init。
    bool IsReady() const;

    // 将多句文本合成为 44100Hz 单声道 PCM；失败返回空 vector。
    std::vector<float> SynthesizeText(const std::string& text);
    // 按句增量合成并回调 PCM 片段；至少产出一段返回 true。
    bool SynthesizeTextStreaming(const std::string& text, const PcmChunkCallback& on_chunk);

private:
    // 在已持锁状态下，完成单句文本的 MeloTTS 推理。
    bool SynthesizeOneSentenceUnlocked(const std::string& sentence, int lang_id,
                                       std::vector<float>& pcm_out);
    // 将语言名转换为推理所需 lang_id。
    int LanguageId(const std::string& language) const;

    mutable std::mutex mutex_;
    MeloTtsConfig cfg_;
    rknn_melotts_context_t ctx_{};
    std::unique_ptr<Lexicon> lexicon_;
    bool ready_ = false;
};
