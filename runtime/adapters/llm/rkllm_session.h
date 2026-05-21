/*
 * adapters/llm/rkllm_session.h — RKLLM 会话封装（init / async run / abort / destroy）。
 */
#pragma once

#include <functional>
#include <string>

#include "rkllm.h"

class RkllmSession {
public:
    using ResultCallback = std::function<void(const char* text_chunk, LLMCallState state)>;

    RkllmSession();
    ~RkllmSession();

    RkllmSession(const RkllmSession&) = delete;
    RkllmSession& operator=(const RkllmSession&) = delete;

    // 加载模型并注册异步回调；param.is_async 置 true。
    int Init(const std::string& model_path, int max_new_tokens, int max_context_len,
             ResultCallback callback);
    // 按 DeepSeek 对话模板拼接 user 文本后异步生成。
    int RunPromptAsync(const std::string& user_text);
    int Abort();
    void Shutdown();
    bool IsInitialized() const { return handle_ != nullptr; }
    bool IsRunning() const;

private:
    static void StaticCallback(RKLLMResult* result, void* userdata, LLMCallState state);

    LLMHandle handle_ = nullptr;
    ResultCallback callback_;
    std::string prompt_buffer_;
};
