/*
 * adapters/tts/tts_planner.cpp
 */
#include "tts_planner.h"

#include <algorithm>
#include <cctype>

#include "split.hpp"

namespace {

// 读取 s[i] 处 UTF-8 码点字节长度。
size_t Utf8CharLenAt(const std::string& s, size_t i) {
    if (i >= s.size()) {
        return 0;
    }
    const unsigned char c = s[i];
    if ((c & 0x80) == 0) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
        return 4;
    }
    return 1;
}

// 开/闭括号对嵌套深度的贡献。
int BracketDepthDelta(const std::string& s, size_t i) {
    if (s.compare(i, 3, "（") == 0 || s.compare(i, 3, "【") == 0) {
        return 1;
    }
    if (s.compare(i, 3, "）") == 0 || s.compare(i, 3, "】") == 0) {
        return -1;
    }
    const char c = s[i];
    if (c == '(' || c == '[' || c == '{') {
        return 1;
    }
    if (c == ')' || c == ']' || c == '}') {
        return -1;
    }
    return 0;
}

// 在 i 处是否为句末标点（不含括注内部）。
bool IsSentenceEndAt(const std::string& s, size_t i) {
    const unsigned char c = s[i];
    if (c == '!' || c == '?' || c == ';' || c == '.') {
        return true;
    }
    if (i + 2 < s.size() && c == 0xEF && static_cast<unsigned char>(s[i + 1]) == 0xBC) {
        const unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
        return c2 == 0x8E || c2 == 0x81 || c2 == 0x9F || c2 == 0x9B;
    }
    if (i + 2 < s.size() && c == 0xE3 && static_cast<unsigned char>(s[i + 1]) == 0x80) {
        const unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
        return c2 == 0x82 || c2 == 0x81;
    }
    return false;
}

// 是否为英文词分隔符。
bool IsWordBoundaryChar(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// 是否包含至少一个可发音字符（字母或 CJK），用于过滤纯标点/空白片段。
bool HasSpeakableContent(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = text[i];
        if (std::isalpha(c)) {
            return true;
        }
        if ((c & 0xE0) == 0xE0 && i + 2 < text.size()) {
            return true;
        }
        i += Utf8CharLenAt(text, i);
    }
    return false;
}

}  // namespace

// 清空待规划缓冲。
void TtsPlanner::Reset() {
    pending_.clear();
    emitted_any_ = false;
    last_feed_tp_ = std::chrono::steady_clock::now();
}

// 写入规划参数。
void TtsPlanner::Configure(const TtsPlannerConfig& cfg) {
    cfg_ = cfg;
    if (cfg_.zh_min_chars < 1) {
        cfg_.zh_min_chars = 1;
    }
    if (cfg_.zh_max_chars < cfg_.zh_min_chars) {
        cfg_.zh_max_chars = cfg_.zh_min_chars;
    }
    if (cfg_.en_min_words < 1) {
        cfg_.en_min_words = 1;
    }
    if (cfg_.en_max_words < cfg_.en_min_words) {
        cfg_.en_max_words = cfg_.en_min_words;
    }
    if (cfg_.fallback_timeout_ms < 100) {
        cfg_.fallback_timeout_ms = 100;
    }
    if (cfg_.short_answer_max_chars < cfg_.zh_max_chars) {
        cfg_.short_answer_max_chars = cfg_.zh_max_chars;
    }
}

// 判断 pending_ 是否以英文为主。
bool TtsPlanner::IsMostlyEnglishLocked() const {
    if (pending_.empty()) {
        return false;
    }
    size_t ascii_letters = 0;
    size_t cjk_chars = 0;
    for (size_t i = 0; i < pending_.size();) {
        const unsigned char c = pending_[i];
        if (std::isalpha(c)) {
            ++ascii_letters;
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xE0 && i + 2 < pending_.size()) {
            ++cjk_chars;
        }
        i += Utf8CharLenAt(pending_, i);
    }
    return ascii_letters > cjk_chars;
}

// 统计 pending_ 前缀英文词数。
size_t TtsPlanner::CountEnglishWords(size_t max_words) const {
    size_t words = 0;
    bool in_word = false;
    for (size_t i = 0; i < pending_.size() && words < max_words; ++i) {
        const unsigned char c = pending_[i];
        if (std::isalpha(c) || c == '\'') {
            if (!in_word) {
                ++words;
                in_word = true;
            }
            continue;
        }
        if (IsWordBoundaryChar(static_cast<char>(c))) {
            in_word = false;
        }
    }
    return words;
}

// 在 pending_ 上找下一个句末标点位置。
size_t TtsPlanner::FindSentenceEndByte() const {
    int depth = 0;
    for (size_t i = 0; i < pending_.size();) {
        const int delta = BracketDepthDelta(pending_, i);
        if (delta != 0) {
            depth += delta;
            if (depth < 0) {
                depth = 0;
            }
            i += Utf8CharLenAt(pending_, i);
            continue;
        }
        if (depth == 0 && IsSentenceEndAt(pending_, i)) {
            return i + Utf8CharLenAt(pending_, i);
        }
        i += Utf8CharLenAt(pending_, i);
    }
    return std::string::npos;
}

// 按 UTF-8 字符数强制切分。
size_t TtsPlanner::ForceSplitByChars(size_t max_chars) const {
    size_t end = 0;
    size_t chars = 0;
    size_t last_space = 0;
    int depth = 0;
    for (size_t i = 0; i < pending_.size() && chars < max_chars;) {
        const int delta = BracketDepthDelta(pending_, i);
        if (delta != 0) {
            depth += delta;
            if (depth < 0) {
                depth = 0;
            }
        } else {
            const unsigned char c = pending_[i];
            if (depth == 0 && IsWordBoundaryChar(static_cast<char>(c))) {
                last_space = i + Utf8CharLenAt(pending_, i);
            }
        }
        i += Utf8CharLenAt(pending_, i);
        end = i;
        ++chars;
    }
    if (depth == 0 && last_space > 0 && last_space < end) {
        return last_space;
    }
    return end;
}

// 尝试从 pending_ 按规则切出片段。
void TtsPlanner::TryEmitSegments(std::vector<std::string>& segments_out) {
    auto emit_candidate = [&](std::string candidate, size_t byte_end) {
        if (byte_end == 0) {
            return false;
        }
        pending_.erase(0, byte_end);
        if (candidate.empty() || !HasSpeakableContent(candidate)) {
            return true;
        }
        segments_out.push_back(std::move(candidate));
        emitted_any_ = true;
        return true;
    };

    for (;;) {
        if (pending_.empty()) {
            return;
        }
        const bool mostly_en = IsMostlyEnglishLocked();
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - last_feed_tp_)
                                    .count();

        const size_t sent_end = FindSentenceEndByte();
        if (sent_end != std::string::npos) {
            const std::string candidate = pending_.substr(0, sent_end);
            if (mostly_en) {
                const size_t words = CountEnglishWords(cfg_.en_max_words + 4);
                if (words >= cfg_.en_max_words ||
                    (words >= cfg_.en_min_words && elapsed_ms >= cfg_.fallback_timeout_ms)) {
                    emit_candidate(std::move(candidate), sent_end);
                    continue;
                }
            } else {
                const size_t char_len = utf8_strlen(candidate);
                if (char_len >= cfg_.zh_max_chars ||
                    (char_len >= cfg_.zh_min_chars && elapsed_ms >= cfg_.fallback_timeout_ms)) {
                    emit_candidate(std::move(candidate), sent_end);
                    continue;
                }
            }
        }

        if (mostly_en) {
            const size_t words = CountEnglishWords(cfg_.en_max_words + 4);
            if (words >= cfg_.en_max_words) {
                size_t byte_end = 0;
                size_t counted = 0;
                bool in_word = false;
                for (size_t i = 0; i < pending_.size() && counted < cfg_.en_max_words; ++i) {
                    const unsigned char c = pending_[i];
                    byte_end = i + 1;
                    if (std::isalpha(c) || c == '\'') {
                        if (!in_word) {
                            ++counted;
                            in_word = true;
                        }
                    } else if (IsWordBoundaryChar(static_cast<char>(c))) {
                        in_word = false;
                        if (counted >= cfg_.en_max_words) {
                            byte_end = i + 1;
                            break;
                        }
                    }
                }
                if (byte_end > 0 && CountEnglishWords(cfg_.en_max_words) >= cfg_.en_min_words) {
                    std::string candidate = pending_.substr(0, byte_end);
                    emit_candidate(std::move(candidate), byte_end);
                    continue;
                }
            }
            if (words >= cfg_.en_min_words && elapsed_ms >= cfg_.fallback_timeout_ms) {
                const size_t byte_end = ForceSplitByChars(cfg_.zh_max_chars * 2);
                if (byte_end > 0 && CountEnglishWords(cfg_.en_max_words) >= cfg_.en_min_words) {
                    std::string candidate = pending_.substr(0, byte_end);
                    emit_candidate(std::move(candidate), byte_end);
                    continue;
                }
            }
            return;
        }

        const size_t buf_chars = utf8_strlen(pending_);
        if (buf_chars >= cfg_.zh_max_chars) {
            const size_t byte_end = ForceSplitByChars(cfg_.zh_max_chars);
            if (byte_end > 0) {
                std::string candidate = pending_.substr(0, byte_end);
                emit_candidate(std::move(candidate), byte_end);
                continue;
            }
        }
        if (buf_chars >= cfg_.zh_min_chars && elapsed_ms >= cfg_.fallback_timeout_ms) {
            const size_t byte_end = ForceSplitByChars(cfg_.zh_min_chars);
            if (byte_end > 0) {
                std::string candidate = pending_.substr(0, byte_end);
                emit_candidate(std::move(candidate), byte_end);
                continue;
            }
        }
        return;
    }
}

// 喂入可见正文增量；达阈值时经 TryEmitSegments 流式切出片段。
void TtsPlanner::Feed(const std::string& visible_delta, std::vector<std::string>& segments_out) {
    if (visible_delta.empty()) {
        return;
    }
    pending_.append(visible_delta);
    last_feed_tp_ = std::chrono::steady_clock::now();
    if (cfg_.short_answer_max_chars > 0 &&
        utf8_strlen(pending_) <= cfg_.short_answer_max_chars) {
        return;
    }
    TryEmitSegments(segments_out);
}

// FINISH 时 flush 尾段。
void TtsPlanner::Flush(std::vector<std::string>& segments_out) {
    if (pending_.empty()) {
        return;
    }
    std::string tail = std::move(pending_);
    pending_.clear();
    if (!tail.empty() && HasSpeakableContent(tail)) {
        segments_out.push_back(std::move(tail));
    }
}
