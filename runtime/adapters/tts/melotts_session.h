/*
 * adapters/tts/melotts_session.h — MeloTTS RKNN 合成会话（encoder/decoder + 词表）。
 */
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lexicon.hpp"
#include "melotts_engine.h"

struct MeloTtsConfig {
    std::string encoder_path;
    std::string decoder_path;
    std::string lexicon_path;
    std::string tokens_path;
    std::string language = "ZH_MIX_EN";
    int speak_id = 1;
    float speed = 1.0f;
    bool disable_bert = true;
};

class MeloTtsSession {
public:
    MeloTtsSession();
    ~MeloTtsSession();

    // 加载 RKNN 与词表；可重复调用，失败返回 false。
    bool Init(const MeloTtsConfig& cfg);
    // 释放 RKNN 与词表。
    void Shutdown();
    bool IsReady() const;

    // 将多句文本合成为 44100Hz 单声道 PCM；失败返回空 vector。
    std::vector<float> SynthesizeText(const std::string& text);

private:
    int LanguageId(const std::string& language) const;

    mutable std::mutex mutex_;
    MeloTtsConfig cfg_;
    rknn_melotts_context_t ctx_{};
    std::unique_ptr<Lexicon> lexicon_;
    bool ready_ = false;
};
