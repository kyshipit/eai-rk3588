/*
 * adapters/tts/tts_ingress.h — LLM 流式输入整理：过滤 thinking、保留 tag 尾、UTF-8 安全。
 */
#pragma once

#include <string>

// 跨 chunk 过滤 <think>…</think>，仅输出可见正文增量；不累积全文。
class TtsIngress {
public:
    // 清空状态，新一轮 rkllm_run 前调用。
    void Reset();

    // 喂入 NORMAL 片段；本次新增的可见正文追加到 visible_delta_out。
    void Feed(const char* chunk, std::string& visible_delta_out);

    // FINISH 后仅吐出 tag 尾残留（若有）；不重复输出已在 Feed 中下发的正文。
    void Flush(std::string& visible_delta_out);

private:
    // 扫描 input 并更新 thinking 状态，可见段写入 delta。
    void ProcessInput(const std::string& input, std::string& visible_delta_out);

    // 若 suffix 可能是未闭合标签前缀，截断到 tag_tail_。
    void HoldPartialTagSuffix(std::string& text, const char* tag);

    bool in_thinking_ = false;
    std::string tag_tail_;
};
