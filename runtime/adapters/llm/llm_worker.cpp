// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 KY (kyshipit)

/*
 * adapters/llm/llm_worker.cpp
 */
#include "llm_worker.h"

#include <chrono>
#include <cstdio>
#include <sys/stat.h>

#include "platform/logging.h"
#include "voice/voice_reply_bridge.h"

namespace {

// 启动预检：缺失或非普通文件时不进入 rkllm_init，避免占 NPU 并误导后续 YOLO Init 日志。
bool LlmModelFileReadable(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

}  // namespace

LlmWorker::LlmWorker() = default;

// 析构时确保会话和异步初始化都被安全回收。
LlmWorker::~LlmWorker() {
    Shutdown();
}

// 写入模型参数与 system_prompt；若上次初始化失败，允许状态回到可重试。
void LlmWorker::Configure(const std::string& model_path, int max_new_tokens, int max_context_len,
                          const std::string& system_prompt) {
    std::lock_guard<std::mutex> lock(mutex_);
    model_path_ = model_path;
    max_new_tokens_ = max_new_tokens;
    max_context_len_ = max_context_len;
    system_prompt_ = system_prompt;
    configured_ = true;
    if (init_state_ == InitState::Failed) {
        init_state_ = InitState::Uninitialized;
    }
}

// C 回调跳板：转发到实例方法，避免在 C API 里直接捕获 C++ 对象。
void LlmWorker::ChunkTrampoline(const char* text_chunk, LLMCallState state, void* user_data) {
    if (!user_data) {
        return;
    }
    static_cast<LlmWorker*>(user_data)->OnLlmChunk(text_chunk, state);
}

// 轻量确保初始化：ready 直接成功，否则触发异步初始化请求。
bool LlmWorker::EnsureInitialized() {
    PollInitState();
    if (IsReady()) {
        return true;
    }
    RequestInitializeAsync();
    return false;
}

// 请求异步初始化（幂等）：未配置/已 ready/正在 init/已失败时均直接返回；缺文件不调 rkllm_init。
void LlmWorker::RequestInitializeAsync() {
    std::string path;
    std::string system_prompt;
    int max_new = 0;
    int max_ctx = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_) {
            LogWarn("LlmWorker: async init skipped (not configured)");
            return;
        }
        if (IsReadyUnlocked() || IsInitializingUnlocked()) {
            return;
        }
        if (init_state_ == InitState::Failed) {
            return;
        }
        path = model_path_;
        system_prompt = system_prompt_;
        max_new = max_new_tokens_;
        max_ctx = max_context_len_;
    }

    if (!LlmModelFileReadable(path)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (IsReadyUnlocked() || IsInitializingUnlocked()) {
            return;
        }
        init_state_ = InitState::Failed;
        LogWarn("LlmWorker: model file lost and failed to load, skip rkllm_init path=%s",
                path.c_str());
        LogSystem("仅视觉模式（对话模型未加载）");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (IsReadyUnlocked() || IsInitializingUnlocked()) {
            return;
        }
        init_state_ = InitState::Initializing;
    }

    LogInfo("LlmWorker: async InitOnce start %s ...", path.c_str());
    LogSystem("正在加载模型，请稍候...");
    init_future_ = std::async(std::launch::async, [this, path, system_prompt, max_new, max_ctx]() {
        return session_.Init(path, max_new, max_ctx, system_prompt, &LlmWorker::ChunkTrampoline,
                             this);
    });
}

// 设置回合结束文本回调（通常由 LlmGreeting 注入）。
void LlmWorker::SetBannerCallback(BannerCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    banner_cb_ = std::move(cb);
}

// 绑定出声旁路装配层。
void LlmWorker::SetVoiceReplyBridge(VoiceReplyBridge* bridge) {
    std::lock_guard<std::mutex> lock(mutex_);
    voice_bridge_ = bridge;
}

// 查询当前是否正在生成。
bool LlmWorker::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return infer_busy_;
}

// 查询模型是否初始化就绪。
bool LlmWorker::IsReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsReadyUnlocked();
}

// 查询是否处于异步初始化中。
bool LlmWorker::IsInitializing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsInitializingUnlocked();
}

// 查询是否已判定加载失败（缺文件或 rkllm_init 失败，本进程内不重试）。
bool LlmWorker::IsLoadFailed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsLoadFailedUnlocked();
}

// 锁内 ready 判定。
bool LlmWorker::IsReadyUnlocked() const {
    return init_state_ == InitState::Ready;
}

// 锁内 initializing 判定。
bool LlmWorker::IsInitializingUnlocked() const {
    return init_state_ == InitState::Initializing;
}

// 锁内 failed 判定。
bool LlmWorker::IsLoadFailedUnlocked() const {
    return init_state_ == InitState::Failed;
}

// 清空所有待处理内容（包括当前已聚合但未发布的 banner）。
void LlmWorker::ClearPendingPrompts() {
    std::lock_guard<std::mutex> lock(mutex_);
    has_pending_ = false;
    pending_prompt_.clear();
    deferred_run_ = false;
    deferred_prompt_.clear();
    pending_text_.clear();
    banner_pending_ = false;
    pending_banner_.clear();
    streamed_chars_ = 0;
    banner_src_ = LlmPromptSource::FaceAppear;
}

// 仅丢弃排队输入，不打断当前正在输出的这一轮。
void LlmWorker::DropQueuedPrompts() {
    std::lock_guard<std::mutex> lock(mutex_);
    has_pending_ = false;
    pending_prompt_.clear();
    deferred_run_ = false;
    deferred_prompt_.clear();
}

// 主线程轮询：voice bridge、发布 banner、投递 deferred/pending 输入。
void LlmWorker::PollDeferred() {
    PollInitState();
    if (voice_bridge_) {
        voice_bridge_->Poll();
    }

    BannerCallback cb;
    std::string banner;
    LlmPromptSource banner_src = LlmPromptSource::FaceAppear;
    std::string prompt;
    LlmPromptSource src = LlmPromptSource::FaceAppear;
    bool run_deferred = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (banner_pending_) {
            banner = pending_banner_;
            pending_banner_.clear();
            banner_pending_ = false;
            banner_src = banner_src_;
            cb = banner_cb_;
        }
        if (deferred_run_) {
            deferred_run_ = false;
            prompt = deferred_prompt_;
            src = deferred_src_;
            deferred_prompt_.clear();
            run_deferred = !prompt.empty();
        }
        if (!run_deferred && has_pending_ && IsReadyUnlocked() && !infer_busy_) {
            prompt = pending_prompt_;
            src = pending_src_;
            pending_prompt_.clear();
            has_pending_ = false;
            run_deferred = !prompt.empty();
        }
    }

    if (cb && !banner.empty()) {
        cb(banner, banner_src, true);
        LogInfo("LlmWorker: turn done (%zu chars)", banner.size());
    }
    if (run_deferred) {
        RunPromptNow(prompt, src);
    }
}

// 源标识字符串化，便于日志定位来源链路。
const char* LlmWorker::SourceName(LlmPromptSource src) {
    switch (src) {
        case LlmPromptSource::FaceAppear:
            return "FaceAppear";
        case LlmPromptSource::FaceReenter:
            return "FaceReenter";
        case LlmPromptSource::Microphone:
            return "Microphone";
        case LlmPromptSource::Button:
            return "Button";
        case LlmPromptSource::Command:
            return "Command";
    }
    return "Unknown";
}

// 轮询异步初始化 future，并在状态转移时输出系统提示。
void LlmWorker::PollInitState() {
    std::future<int> done_future;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (init_state_ != InitState::Initializing || !init_future_.valid()) {
            return;
        }
        if (init_future_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            return;
        }
        done_future = std::move(init_future_);
    }

    const int ret = done_future.get();

    std::lock_guard<std::mutex> lock(mutex_);
    if (ret == 0) {
        init_state_ = InitState::Ready;
        LogInfo("LlmWorker: rkllm_init ok (async, model stays loaded until process exit)");
        LogSystem("对话模型已就绪，人脸稳定后可输入");
    } else {
        init_state_ = InitState::Failed;
        LogWarn("LlmWorker: rkllm_init failed (async ret=%d)", ret);
        LogSystem("仅视觉模式（对话模型未加载）");
    }
}

// 等待推理线程结束，Shutdown 前必须 join 避免与 rkllm_destroy 并发。
void LlmWorker::JoinInferThread() {
    if (infer_thread_.joinable()) {
        infer_thread_.join();
    }
}

// 立即尝试发送一条输入：在独立线程上 rkllm_run，主循环不被阻塞。
bool LlmWorker::RunPromptNow(const std::string& user_text, LlmPromptSource src) {
    if (!EnsureInitialized()) {
        LogInfo("LlmWorker: submit deferred until async init ready src=%s", SourceName(src));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (infer_busy_) {
            return false;
        }
        infer_busy_ = true;
        pending_text_.clear();
        streamed_chars_ = 0;
        current_src_ = src;
    }

    JoinInferThread();
    infer_thread_ = std::thread([this, user_text]() {
        LlmStdoutStreamGuard stream_guard;
        SessionStdoutWrite("AI> ");
        VoiceReplyBridge* bridge = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reply_accumulator_.clear();
            bridge = voice_bridge_;
            if (bridge) {
                session_.SetReplyAccumulator(&reply_accumulator_);
                bridge->OnRunStarted();
            } else {
                session_.SetReplyAccumulator(nullptr);
            }
        }
        const int ret = session_.RunPromptSync(user_text);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_.SetReplyAccumulator(nullptr);
            reply_accumulator_.clear();
            infer_busy_ = false;
        }
        if (ret != 0) {
            LogWarn("LlmWorker: rkllm_run failed (%d)", ret);
        }
    });
    return true;
}

// 门控入口：允许时直接跑，不允许或忙时走“只保留一条”排队策略。
bool LlmWorker::SubmitPrompt(const std::string& user_text, LlmPromptSource src, bool gate_open) {
    if (!gate_open) {
        return false;
    }
    if (user_text.empty()) {
        return false;
    }
    if (voice_bridge_) {
        voice_bridge_->BeginTurn();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (IsLoadFailedUnlocked()) {
            return false;
        }
    }
    if (RunPromptNow(user_text, src)) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (infer_busy_ || IsInitializingUnlocked() || !IsReadyUnlocked()) {
        has_pending_ = true;
        pending_prompt_ = user_text;
        pending_src_ = src;
        if (infer_busy_) {
            LogInfo("LlmWorker: queued one prompt (busy) src=%s", SourceName(src));
        } else {
            LogInfo("LlmWorker: queued one prompt (init pending) src=%s", SourceName(src));
        }
    }
    return false;
}

// 请求 rkllm_abort，使推理线程尽快从 RunPromptSync 返回。
void LlmWorker::RequestAbortGeneration() {
    session_.Abort();
    if (voice_bridge_) {
        voice_bridge_->Cancel();
    }
}

// 关闭 worker：先 abort 再 join 推理线程，最后 destroy 会话。
void LlmWorker::Shutdown() {
    {
        std::future<int> pending_init;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (init_future_.valid()) {
                pending_init = std::move(init_future_);
            }
        }
        if (pending_init.valid()) {
            pending_init.wait();
        }
    }

    session_.Abort();
    JoinInferThread();
    session_.Shutdown();
    if (voice_bridge_) {
        voice_bridge_->Reset();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    init_state_ = InitState::Uninitialized;
    infer_busy_ = false;
    pending_text_.clear();
    streamed_chars_ = 0;
    has_pending_ = false;
    pending_prompt_.clear();
    deferred_run_ = false;
    deferred_prompt_.clear();
    banner_pending_ = false;
    pending_banner_.clear();
}

// RKLLM 回调处理：转发 chunk 给 bridge，FINISH 时安排 deferred 下一句。
void LlmWorker::OnLlmChunk(const char* text_chunk, LLMCallState state) {
    std::string queued_prompt;
    LlmPromptSource queued_src = LlmPromptSource::FaceAppear;
    bool has_queued = false;

    if (voice_bridge_) {
        voice_bridge_->OnLlmChunk(text_chunk, state);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state == RKLLM_RUN_FINISH) {
            pending_text_.clear();
            streamed_chars_ = 0;
            if (has_pending_) {
                queued_prompt = pending_prompt_;
                queued_src = pending_src_;
                has_queued = true;
                has_pending_ = false;
                pending_prompt_.clear();
            }
        } else if (state == RKLLM_RUN_ERROR) {
            LogWarn("LlmWorker: RKLLM_RUN_ERROR");
            pending_text_.clear();
            streamed_chars_ = 0;
            has_pending_ = false;
            pending_prompt_.clear();
        }
    }

    if (has_queued && !queued_prompt.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        deferred_run_ = true;
        deferred_prompt_ = std::move(queued_prompt);
        deferred_src_ = queued_src;
    }
}
