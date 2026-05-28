/*
 * adapters/tts/tts_text_sanitizer.cpp
 */
#include "tts_text_sanitizer.h"

#include <cctype>

namespace {

// 跳过 markdown 常见符号，保留可读字符与空白。
void StripMarkdownLike(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '#' || c == '*' || c == '`' || c == '|') {
            continue;
        }
        out.push_back(c);
    }
    s.swap(out);
}

// 合并连续空白为单个空格。
void CollapseWhitespace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_space = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!in_space && !out.empty()) {
                out.push_back(' ');
                in_space = true;
            }
        } else {
            out.push_back(c);
            in_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    s.swap(out);
}

}  // namespace

// 返回适合送入 MeloTTS 的文本；空串表示无需播报。
std::string TtsTextSanitizer::Sanitize(const std::string& text, int max_chars) {
    if (text.empty()) {
        return std::string();
    }
    std::string s = text;
    StripMarkdownLike(s);
    CollapseWhitespace(s);
    if (s.empty()) {
        return std::string();
    }
    if (max_chars > 0 && static_cast<int>(s.size()) > max_chars) {
        s.resize(static_cast<size_t>(max_chars));
    }
    return s;
}
