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

    // 加载 .rkllm；注册 StaticCallback；chunk_fn 在推理回调里被调用（NORMAL/FINISH/ERROR）。
    int Init(const std::string& model_path, int max_new_tokens, int max_context_len,
             RkllmChunkFn chunk_fn, void* user_data);
    // 设置拼在 User 段末尾的约束文案（空串则 User 段仅含终端输入）。
    void SetUserPromptPrefix(const std::string& user_prompt_prefix);
    // rkllm_run 同步推理；prompt 保存在成员 buffer，回调线程直出 stdout。
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
    static void StaticCallback(RKLLMResult* result, void* userdata, LLMCallState state);

    static constexpr uint32_t kMagic = 0x524B4C4Du;

    LLMHandle handle_ = nullptr;
    RkllmChunkFn chunk_fn_ = nullptr;
    void* chunk_user_data_ = nullptr;
    uint32_t magic_ = kMagic;
    std::string user_prompt_prefix_;
    std::string prompt_buffer_;
    // 推理入参保存在成员上，避免库侧延迟访问栈对象。
    RKLLMInput run_input_;
    RKLLMInferParam run_infer_param_;
    std::string* reply_accumulator_ = nullptr;
    std::mutex reply_mutex_;
};
