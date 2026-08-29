// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 KY (kyshipit)

/*
 * adapters/llm/rkllm_session.h — RKLLM 会话封装（init / sync run / abort / destroy）。
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "rkllm.h"

// librkllmrt 回调线程调用的 C 函数指针；禁止在回调内再调 rkllm_*。
typedef void (*RkllmChunkFn)(const char* text_chunk, LLMCallState state, void* user_data);

class RkllmSession {
public:
    RkllmSession();
    ~RkllmSession();

    RkllmSession(const RkllmSession&) = delete;
    RkllmSession& operator=(const RkllmSession&) = delete;

    // 加载 .rkllm；可选 system_prompt 经 rkllm_set_chat_template 注入；注册 StaticCallback。
    int Init(const std::string& model_path, int max_new_tokens, int max_context_len,
             const std::string& system_prompt, RkllmChunkFn chunk_fn, void* user_data);
    // rkllm_run 同步推理；有自定义 ChatML 时不设 role，否则 role=user 走内置模板。
    int RunPromptSync(const std::string& user_text);
    int Abort();
    // Abort 后等待推理结束再 destroy，避免与回调并发。
    void Shutdown();
    bool IsInitialized() const { return handle_ != nullptr; }
    bool IsRunning() const;

    // TTS 开启时：NORMAL 与 printf 同步追加，供流式/FINISH 播报。
    void SetReplyAccumulator(std::string* accumulator);
    // 取走并清空累积回复（线程安全）。
    std::string TakeReplyAccumulator();

private:
    // 返回 0 继续生成；与新版 LLMResultCallback 签名一致。
    static int StaticCallback(RKLLMResult* result, void* userdata, LLMCallState state);

    static constexpr uint32_t kMagic = 0x524B4C4Du;

    LLMHandle handle_ = nullptr;
    RkllmChunkFn chunk_fn_ = nullptr;
    void* chunk_user_data_ = nullptr;
    uint32_t magic_ = kMagic;
    std::string prompt_buffer_;
    bool custom_chat_template_ = false;
    // 推理入参保存在成员上，避免库侧延迟访问栈对象。
    RKLLMInput run_input_;
    RKLLMInferParam run_infer_param_;
    std::string* reply_accumulator_ = nullptr;
    std::mutex reply_mutex_;
};
