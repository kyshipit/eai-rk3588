/*
 * adapters/tts/tts_text_sanitizer.h — 播报前文本截断（thinking 由 TtsIngress 过滤）。
 */
#pragma once

#include <string>

class TtsTextSanitizer {
public:
    // max_chars<=0 表示不截断；不做 markdown/括号等改写。
    static std::string Sanitize(const std::string& text, int max_chars);
};
