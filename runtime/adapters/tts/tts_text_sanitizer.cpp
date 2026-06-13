/*
 * adapters/tts/tts_text_sanitizer.cpp
 */
#include "tts_text_sanitizer.h"

namespace {

// 去掉 emoji（UTF-8 四字节码点）与控制符，避免 Melo 句首 PCM 异常。
std::string StripNonSpeakable(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = text[i];
        if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
            i += 4;
            continue;
        }
        if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
            out.append(text, i, 3);
            i += 3;
            continue;
        }
        if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            out.append(text, i, 2);
            i += 2;
            continue;
        }
        if (c < 0x20 && c != ' ' && c != '\n') {
            ++i;
            continue;
        }
        out += static_cast<char>(c);
        ++i;
    }
    return out;
}

}  // namespace

// 返回适合送入 MeloTTS 的文本；滤 emoji 并按 max_chars 截断。
std::string TtsTextSanitizer::Sanitize(const std::string& text, int max_chars) {
    std::string cleaned = StripNonSpeakable(text);
    if (cleaned.empty()) {
        return std::string();
    }
    if (max_chars > 0 && static_cast<int>(cleaned.size()) > max_chars) {
        return cleaned.substr(0, static_cast<size_t>(max_chars));
    }
    return cleaned;
}
