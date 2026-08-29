// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 KY (kyshipit)

/*
 * voice/tts_text_sanitizer.cpp
 */
#include "tts_text_sanitizer.h"

#include "split.hpp"

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

// 按 UTF-8 字符数截断，避免在码点中间切断。
std::string TruncateUtf8Chars(const std::string& text, size_t max_chars) {
    if (max_chars == 0 || utf8_strlen(text) <= max_chars) {
        return text;
    }
    size_t char_count = 0;
    size_t byte_end = 0;
    for (size_t i = 0; i < text.size() && char_count < max_chars;) {
        const unsigned char c = text[i];
        size_t advance = 1;
        if ((c & 0x80) == 0) {
            advance = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            advance = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
            advance = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
            advance = 4;
        } else {
            break;
        }
        ++char_count;
        i += advance;
        byte_end = i;
    }
    return text.substr(0, byte_end);
}

}  // namespace

// 返回适合送入 MeloTTS 的文本；滤 emoji 并按 max_chars（字符数）截断。
std::string TtsTextSanitizer::Sanitize(const std::string& text, int max_chars) {
    std::string cleaned = StripNonSpeakable(text);
    if (cleaned.empty()) {
        return std::string();
    }
    if (max_chars > 0 && static_cast<size_t>(max_chars) < utf8_strlen(cleaned)) {
        return TruncateUtf8Chars(cleaned, static_cast<size_t>(max_chars));
    }
    return cleaned;
}
