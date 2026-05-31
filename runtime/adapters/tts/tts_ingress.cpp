/*
 * adapters/tts/tts_ingress.cpp
 */
#include "tts_ingress.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

constexpr const char* kOpenTag = "<think>";
constexpr const char* kCloseTag = "</think>";

// 不区分大小写查找子串；找不到返回 npos。
size_t FindTagInsensitive(const std::string& haystack, const char* tag, size_t from) {
    const size_t tag_len = std::strlen(tag);
    if (tag_len == 0 || from >= haystack.size()) {
        return std::string::npos;
    }
    for (size_t i = from; i + tag_len <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < tag_len; ++j) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(tag[j]))) {
                match = false;
                break;
            }
        }
        if (match) {
            return i;
        }
    }
    return std::string::npos;
}

}  // namespace

// 清空 thinking 状态与 tag 尾。
void TtsIngress::Reset() {
    in_thinking_ = false;
    tag_tail_.clear();
}

// 保留 text 末尾可能是 tag 前缀的部分到 tag_tail_。
void TtsIngress::HoldPartialTagSuffix(std::string& text, const char* tag) {
    const size_t tag_len = std::strlen(tag);
    if (tag_len <= 1 || text.empty()) {
        return;
    }
    const size_t max_hold = tag_len - 1;
    const size_t hold = std::min(max_hold, text.size());
    for (size_t n = hold; n >= 1; --n) {
        const std::string suffix = text.substr(text.size() - n);
        bool prefix_match = true;
        for (size_t j = 0; j < suffix.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(suffix[j])) !=
                std::tolower(static_cast<unsigned char>(tag[j]))) {
                prefix_match = false;
                break;
            }
        }
        if (prefix_match) {
            tag_tail_ = suffix;
            text.resize(text.size() - n);
            return;
        }
        if (n == 1) {
            break;
        }
    }
}

// 扫描并跳过 thinking 块，可见段仅追加到 delta（不累积全文缓冲）。
void TtsIngress::ProcessInput(const std::string& input, std::string& visible_delta_out) {
    size_t i = 0;
    while (i < input.size()) {
        if (in_thinking_) {
            const size_t close_pos = FindTagInsensitive(input, kCloseTag, i);
            if (close_pos == std::string::npos) {
                std::string tail = input.substr(i);
                HoldPartialTagSuffix(tail, kCloseTag);
                return;
            }
            i = close_pos + std::strlen(kCloseTag);
            in_thinking_ = false;
            continue;
        }

        const size_t open_pos = FindTagInsensitive(input, kOpenTag, i);
        if (open_pos == std::string::npos) {
            std::string visible = input.substr(i);
            HoldPartialTagSuffix(visible, kOpenTag);
            if (!visible.empty()) {
                visible_delta_out.append(visible);
            }
            return;
        }
        if (open_pos > i) {
            visible_delta_out.append(input.substr(i, open_pos - i));
        }
        i = open_pos + std::strlen(kOpenTag);
        in_thinking_ = true;
    }
}

// 喂入 RKLLM NORMAL 文本块。
void TtsIngress::Feed(const char* chunk, std::string& visible_delta_out) {
    visible_delta_out.clear();
    if (!chunk || chunk[0] == '\0') {
        return;
    }
    std::string input = tag_tail_;
    tag_tail_.clear();
    input += chunk;
    ProcessInput(input, visible_delta_out);
}

// 仅输出 tag 尾残留；已在 Feed 中下发的正文不再重复输出。
void TtsIngress::Flush(std::string& visible_delta_out) {
    visible_delta_out.clear();
    if (in_thinking_) {
        tag_tail_.clear();
        in_thinking_ = false;
        return;
    }
    if (!tag_tail_.empty()) {
        visible_delta_out = tag_tail_;
        tag_tail_.clear();
    }
}
