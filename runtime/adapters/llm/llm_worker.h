/*
 * adapters/llm/llm_worker.h — RKLLM 常驻适配器：InitOnce + 按需 SubmitPrompt。
 */
#pragma once

#include <deque>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>

#include "adapters/tts/tts_ingress.h"
#include "adapters/tts/tts_planner.h"
#include "rkllm_session.h"

class TtsWorker;

enum class LlmPromptSource {
    FaceAppear,
    FaceReenter,
    Microphone,
    Button,
    Command,
};

class LlmWorker {
public:
    using BannerCallback =
        std::function<void(const std::string& line, LlmPromptSource src, bool is_final)>;

    LlmWorker();
    ~LlmWorker();

    // 写入模型路径、生成参数与 User 段末尾约束；不触发立即初始化。
    void Configure(const std::string& model_path, int max_new_tokens, int max_context_len,
                   const std::string& user_prompt_prefix);
    // 快速确保初始化（内部可触发异步初始化请求）。
    bool EnsureInitialized();
    // 显式请求异步初始化（幂等）。
    void RequestInitializeAsync();
    // 设置一轮回复完成后的文本回调。
    void SetBannerCallback(BannerCallback cb);
    // 绑定 TTS；NORMAL 流式 chunk event 经 Ingress/Planner 旁路。
    void SetTtsWorker(TtsWorker* tts);
    // 配置正式回答规划参数。
    void ConfigureTtsPlanner(const TtsPlannerConfig& cfg);
    // 是否在本轮 rkllm 结束后将累积正文提交 TTS。
    void SetTtsEnabled(bool enabled);

    // gate_open=false 时不提交；忙时可缓存一句待 gate 恢复后发送。
    bool SubmitPrompt(const std::string& user_text, LlmPromptSource src, bool gate_open);
    // 清空全部待处理状态（用于 reset）。
    void ClearPendingPrompts();
    // 仅丢弃排队输入，保留正在生成中的文本收尾。
    void DropQueuedPrompts();

    // 主线程每帧调用：处理 FINISH 后衔接的下一句排队（禁止在回调内 rkllm_run）。
    void PollDeferred();

    // 主动关闭会话并重置内部状态。
    void Shutdown();
    // 中断当前 rkllm_run（退出/ESC 时调用，避免 JoinInferThread 长时间阻塞）。
    void RequestAbortGeneration();

    // 状态查询接口。
    bool IsReady() const;
    bool IsInitializing() const;
    bool IsLoadFailed() const;
    bool IsBusy() const;

private:
    // 回调线程投递的 TTS 事件（正文 chunk / FINISH 刷尾）。
    struct TtsEvent {
        uint64_t session_id = 0;
        LLMCallState state = RKLLM_RUN_NORMAL;
        std::string chunk;
    };

    static void ChunkTrampoline(const char* text_chunk, LLMCallState state, void* user_data);
    void OnLlmChunk(const char* text_chunk, LLMCallState state);
    // 在主线程消费回调投递的 TTS 事件，避免回调线程执行分块逻辑。
    void DrainDeferredTtsEvents();
    bool RunPromptNow(const std::string& user_text, LlmPromptSource src);
    static const char* SourceName(LlmPromptSource src);
    bool IsReadyUnlocked() const;
    bool IsInitializingUnlocked() const;
    bool IsLoadFailedUnlocked() const;
    // 当前 run 的 TTS 会话是否仍为最新（用于丢弃旧会话 chunk）。
    bool IsCurrentTtsSessionLiveUnlocked() const;
    void PollInitState();
    void JoinInferThread();

    enum class InitState {
        Uninitialized,
        Initializing,
        Ready,
        Failed
    };

    RkllmSession session_;
    TtsWorker* tts_ = nullptr;
    bool tts_enabled_ = false;
    std::string reply_accumulator_;
    TtsIngress tts_ingress_;
    TtsPlanner tts_planner_;
    BannerCallback banner_cb_;
    mutable std::mutex mutex_;
    std::string model_path_;
    int max_new_tokens_ = 0;
    int max_context_len_ = 0;
    std::string user_prompt_prefix_;
    bool configured_ = false;
    InitState init_state_ = InitState::Uninitialized;
    std::future<int> init_future_;
    std::thread infer_thread_;
    std::string pending_text_;
    bool infer_busy_ = false;
    bool has_pending_ = false;
    std::string pending_prompt_;
    LlmPromptSource pending_src_ = LlmPromptSource::FaceAppear;
    bool deferred_run_ = false;
    std::string deferred_prompt_;
    LlmPromptSource deferred_src_ = LlmPromptSource::FaceAppear;
    bool banner_pending_ = false;
    std::string pending_banner_;
    LlmPromptSource banner_src_ = LlmPromptSource::FaceAppear;
    LlmPromptSource current_src_ = LlmPromptSource::FaceAppear;
    size_t streamed_chars_ = 0;
    std::deque<TtsEvent> tts_events_;
    uint64_t desired_tts_session_id_ = 1;
    uint64_t current_tts_session_id_ = 1;
};
