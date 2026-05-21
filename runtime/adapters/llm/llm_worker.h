/*
 * adapters/llm/llm_worker.h — 异步问候：封装 RkllmSession，向 LlmGreeting 回写 banner。
 */
#pragma once

#include <functional>
#include <mutex>
#include <string>

#include "rkllm_session.h"

class LlmWorker {
public:
    using BannerCallback = std::function<void(const std::string& line)>;

    LlmWorker();
    ~LlmWorker();

    // 初始化 RKLLM；失败返回非 0。
    int Init(const std::string& model_path, int max_new_tokens, int max_context_len);
    void SetBannerCallback(BannerCallback cb);
    // 人脸稳定后触发一次问候（忙则跳过）。
    void RequestGreeting(const std::string& user_prompt);
    void Shutdown();

    bool IsReady() const { return session_.IsInitialized(); }

private:
    void OnLlmChunk(const char* text_chunk, LLMCallState state);

    RkllmSession session_;
    BannerCallback banner_cb_;
    std::mutex mutex_;
    std::string pending_text_;
    bool infer_busy_ = false;
};
