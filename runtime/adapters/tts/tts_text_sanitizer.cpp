/*
 * adapters/tts/tts_text_sanitizer.cpp
 */
#include "tts_text_sanitizer.h"

// 返回适合送入 MeloTTS 的文本；仅做 max_chars 截断，thinking 由 TtsIngress 过滤。
std::string TtsTextSanitizer::Sanitize(const std::string& text, int max_chars) {
    if (text.empty()) {
        return std::string();
    }
    if (max_chars > 0 && static_cast<int>(text.size()) > max_chars) {
        return text.substr(0, static_cast<size_t>(max_chars));
    }
    return text;
}
