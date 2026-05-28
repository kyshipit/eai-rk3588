/*
 * adapters/tts/tts_text_sanitizer.h — 播报前清洗 AI> 文案（去 markdown 等）。
 */
#pragma once

#include <string>

class TtsTextSanitizer {
public:
    // max_chars<=0 表示不截断。
    static std::string Sanitize(const std::string& text, int max_chars);
};
