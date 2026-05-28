/*
 * adapters/tts/melotts_session.cpp
 */
#include "melotts_session.h"

#include <cstring>
#include <map>

#include "melotts_process.h"
#include "platform/logging.h"
#include "split.hpp"

namespace {

// 与例程 main.cc 一致：在音素序列间插入 blank。
std::vector<int64_t> Intersperse(const std::vector<int>& lst, int item) {
    std::vector<int64_t> result(lst.size() * 2 + 1, item);
    for (size_t i = 1; i < result.size(); i += 2) {
        result[i] = lst[i / 2];
    }
    return result;
}

// 将向量 pad/trim 到 MAX_LENGTH。
void PadOrTrim(std::vector<int64_t>& vec, int max_size) {
    if (static_cast<int>(vec.size()) < max_size) {
        vec.resize(static_cast<size_t>(max_size), 0);
    } else if (static_cast<int>(vec.size()) > max_size) {
        vec.resize(static_cast<size_t>(max_size));
    }
}

const std::map<std::string, int> kLanguageIdMap = {
    {"ZH", 0},       {"JP", 1},       {"EN", 2}, {"ZH_MIX_EN", 3},
    {"KR", 4},       {"SP", 5},       {"ES", 5}, {"FR", 6},
};

}  // namespace

// 默认空会话。
MeloTtsSession::MeloTtsSession() {
    std::memset(&ctx_, 0, sizeof(ctx_));
}

// 析构时释放已加载的 RKNN。
MeloTtsSession::~MeloTtsSession() {
    Shutdown();
}

// 将语言名映射为模型 lang_id；未知语言回退 ZH_MIX_EN。
int MeloTtsSession::LanguageId(const std::string& language) const {
    std::string lang = language;
    if (lang == "ZH") {
        lang = "ZH_MIX_EN";
    }
    const auto it = kLanguageIdMap.find(lang);
    if (it != kLanguageIdMap.end()) {
        return it->second;
    }
    return 3;
}

// 初始化双 RKNN 与 Lexicon。
bool MeloTtsSession::Init(const MeloTtsConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    Shutdown();
    cfg_ = cfg;
    if (cfg_.encoder_path.empty() || cfg_.decoder_path.empty()) {
        LogWarn("MeloTtsSession: empty model path");
        return false;
    }
    try {
        lexicon_.reset(new Lexicon(cfg_.lexicon_path, cfg_.tokens_path));
    } catch (...) {
        LogWarn("MeloTtsSession: lexicon load failed lex=%s tokens=%s",
                cfg_.lexicon_path.c_str(), cfg_.tokens_path.c_str());
        lexicon_.reset();
        return false;
    }
    if (init_melotts_model(cfg_.encoder_path.c_str(), &ctx_.encoder_context) != 0) {
        LogWarn("MeloTtsSession: encoder init failed %s", cfg_.encoder_path.c_str());
        lexicon_.reset();
        return false;
    }
    if (init_melotts_model(cfg_.decoder_path.c_str(), &ctx_.decoder_context) != 0) {
        LogWarn("MeloTtsSession: decoder init failed %s", cfg_.decoder_path.c_str());
        release_melotts_model(&ctx_.encoder_context);
        lexicon_.reset();
        return false;
    }
    ready_ = true;
    LogInfo("MeloTtsSession: ready encoder=%s", cfg_.encoder_path.c_str());
    return true;
}

// 释放模型与词表。
void MeloTtsSession::Shutdown() {
    if (ready_) {
        release_melotts_model(&ctx_.encoder_context);
        release_melotts_model(&ctx_.decoder_context);
    }
    std::memset(&ctx_, 0, sizeof(ctx_));
    lexicon_.reset();
    ready_ = false;
}

// 是否已完成 Init。
bool MeloTtsSession::IsReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
}

// 分句后逐句 RKNN 推理并拼接 PCM。
std::vector<float> MeloTtsSession::SynthesizeText(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || !lexicon_ || text.empty()) {
        return {};
    }
    const int lang_id = LanguageId(cfg_.language);
    auto sentences = split_sentence(text, 40, cfg_.language);
    std::vector<float> output_wav_data;
    for (std::string s : sentences) {
        if (s.empty()) {
            continue;
        }
        s = "_" + s + "_";
        std::vector<int> phones_bef;
        std::vector<int> tones_bef;
        lexicon_->convert(s, phones_bef, tones_bef);
        if (phones_bef.empty()) {
            continue;
        }
        std::vector<int> lang_ids_bef(phones_bef.size(), lang_id);
        std::vector<int64_t> phones = Intersperse(phones_bef, 0);
        std::vector<int64_t> tones = Intersperse(tones_bef, 0);
        std::vector<int64_t> lang_ids = Intersperse(lang_ids_bef, 0);
        const int64_t phone_len = static_cast<int64_t>(phones.size());
        PadOrTrim(tones, MAX_LENGTH);
        PadOrTrim(phones, MAX_LENGTH);
        PadOrTrim(lang_ids, MAX_LENGTH);
        std::vector<float> output_data(PREDICTED_LENGTHS_MAX * PREDICTED_BATCH);
        const int output_lengths = inference_melotts_model(
            &ctx_, phones, phone_len, tones, lang_ids, cfg_.speak_id, cfg_.speed,
            cfg_.disable_bert, output_data);
        if (output_lengths < 0) {
            LogWarn("MeloTtsSession: inference failed ret=%d", output_lengths);
            continue;
        }
        const int actual_size = output_lengths * PREDICTED_BATCH;
        output_wav_data.insert(output_wav_data.end(), output_data.begin(),
                               output_data.begin() + actual_size);
    }
    return output_wav_data;
}
