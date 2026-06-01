/*
 * adapters/tts/tts_planner.h — 正式回答语言感知规划：中文字符数、英文词数、标点优先。
 */
#pragma once

#include <chrono>
#include <string>
#include <vector>

struct TtsPlannerConfig {
    size_t zh_min_chars = 8;
    size_t zh_max_chars = 15;
    size_t en_min_words = 4;
    size_t en_max_words = 8;
    int fallback_timeout_ms = 600;
};

// 接收 TtsIngress 可见增量，按语言规则规划正式回答片段；禁止英文单词级输出。
class TtsPlanner {
public:
    // 清空待规划缓冲。
    void Reset();

    // 写入规划参数。
    void Configure(const TtsPlannerConfig& cfg);

    // 喂入可见正文增量；达阈值时流式切出片段写入 segments_out，尾段留待 Flush。
    void Feed(const std::string& visible_delta, std::vector<std::string>& segments_out);

    // FINISH 时 flush 尾段。
    void Flush(std::vector<std::string>& segments_out);

private:
    // 尝试从 pending_ 按规则切出片段。
    void TryEmitSegments(std::vector<std::string>& segments_out);

    // 判断 pending_ 是否以英文为主（ASCII 字母占比）。
    bool IsMostlyEnglishLocked() const;

    // 统计 pending_ 前缀英文词数（到 max_words 为止）。
    size_t CountEnglishWords(size_t max_words) const;

    // 在 pending_ 上找下一个句末标点位置（字节 end，不含）。
    size_t FindSentenceEndByte() const;

    // 按 UTF-8 字符数强制切分，返回字节 end。
    size_t ForceSplitByChars(size_t max_chars) const;

    TtsPlannerConfig cfg_;
    std::string pending_;
    bool emitted_any_ = false;
    std::chrono::steady_clock::time_point last_feed_tp_ = std::chrono::steady_clock::now();
};
