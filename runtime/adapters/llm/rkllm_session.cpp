/*
 * adapters/llm/rkllm_session.cpp
 */
#include "rkllm_session.h"

#include <cstdio>
#include <chrono>
#include <cstring>
#include <pthread.h>
#include <thread>

#include "platform/logging.h"

namespace {
RkllmSession* g_active_session = nullptr;
constexpr const char* kPromptPrefix = "<｜begin▁of▁sentence｜><｜User｜>";
constexpr const char* kPromptPostfix = "<｜Assistant｜>";
}  // namespace

// 默认构造；句柄为空。
RkllmSession::RkllmSession() {
    std::memset(&run_input_, 0, sizeof(run_input_));
    std::memset(&run_infer_param_, 0, sizeof(run_infer_param_));
}

// 析构时释放 RKLLM 句柄。
RkllmSession::~RkllmSession() {
    Shutdown();
}

// 初始化 RKLLM：设置默认参数、注册库回调、保存上层 chunk_fn。
int RkllmSession::Init(const std::string& model_path, int max_new_tokens, int max_context_len,
                         RkllmChunkFn chunk_fn, void* user_data) {
    LogDebug("LLM_DBG RkllmSession::Init enter this=%p path=%s max_new=%d max_ctx=%d chunk_fn=%p user=%p",
             static_cast<void*>(this), model_path.c_str(), max_new_tokens, max_context_len,
             reinterpret_cast<void*>(chunk_fn), user_data);
    Shutdown();
    chunk_fn_ = chunk_fn;
    chunk_user_data_ = user_data;
    magic_ = kMagic;
    g_active_session = this;

    RKLLMParam param = rkllm_createDefaultParam();
    param.model_path = model_path.c_str();
    param.top_k = 1;
    param.top_p = 0.95f;
    param.temperature = 0.8f;
    param.repeat_penalty = 1.1f;
    param.frequency_penalty = 0.0f;
    param.presence_penalty = 0.0f;
    // 仅当配置 >0 时覆盖 rkllm_createDefaultParam 的 max_new_tokens / max_context_len。
    if (max_new_tokens > 0) {
        param.max_new_tokens = max_new_tokens;
    }
    if (max_context_len > 0) {
        param.max_context_len = max_context_len;
    }
    param.skip_special_token = true;
    param.extend_param.base_domain_id = 0;

    LogDebug("LLM_DBG RkllmSession::Init calling rkllm_init pthread=%p",
             reinterpret_cast<void*>(pthread_self()));
    const int ret = rkllm_init(&handle_, &param, &RkllmSession::StaticCallback);
    if (ret != 0) {
        LogDebug("LLM_DBG RkllmSession::Init rkllm_init failed ret=%d", ret);
        g_active_session = nullptr;
        handle_ = nullptr;
        chunk_fn_ = nullptr;
        chunk_user_data_ = nullptr;
    } else {
        LogDebug("LLM_DBG RkllmSession::Init ok handle=%p g_active=%p",
                 handle_, static_cast<void*>(g_active_session));
    }
    return ret;
}

// rkllm_run 阻塞至本轮结束；NORMAL 在 StaticCallback 内直接 printf。
int RkllmSession::RunPromptSync(const std::string& user_text) {
    if (!handle_ || !chunk_fn_) {
        return -1;
    }
    prompt_buffer_ = std::string(kPromptPrefix) + user_text + kPromptPostfix;

    std::memset(&run_input_, 0, sizeof(run_input_));
    run_input_.input_type = RKLLM_INPUT_PROMPT;
    run_input_.prompt_input = prompt_buffer_.c_str();

    std::memset(&run_infer_param_, 0, sizeof(run_infer_param_));
    run_infer_param_.mode = RKLLM_INFER_GENERATE;

    // userdata 传 NULL，回调经 g_active_session 定位当前会话实例。
    return rkllm_run(handle_, &run_input_, &run_infer_param_, nullptr);
}

// 中止当前推理任务。
int RkllmSession::Abort() {
    LogDebug("LLM_DBG RkllmSession::Abort handle=%p pthread=%p", handle_,
             reinterpret_cast<void*>(pthread_self()));
    if (!handle_) {
        return -1;
    }
    const int ret = rkllm_abort(handle_);
    LogDebug("LLM_DBG RkllmSession::Abort ret=%d", ret);
    return ret;
}

// 等待推理结束并销毁句柄，清空全局 active 指针。
void RkllmSession::Shutdown() {
    LogDebug("LLM_DBG RkllmSession::Shutdown enter this=%p handle=%p pthread=%p",
             static_cast<void*>(this), handle_, reinterpret_cast<void*>(pthread_self()));
    if (g_active_session == this) {
        g_active_session = nullptr;
    }
    if (!handle_) {
        chunk_fn_ = nullptr;
        chunk_user_data_ = nullptr;
        return;
    }
    Abort();
    for (int i = 0; i < 300; ++i) {
        if (!IsRunning()) {
            LogDebug("LLM_DBG RkllmSession::Shutdown wait done i=%d", i);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LLMHandle tmp = handle_;
    handle_ = nullptr;
    LogDebug("LLM_DBG RkllmSession::Shutdown calling rkllm_destroy tmp=%p", tmp);
    rkllm_destroy(tmp);
    chunk_fn_ = nullptr;
    chunk_user_data_ = nullptr;
    std::memset(&run_input_, 0, sizeof(run_input_));
    std::memset(&run_infer_param_, 0, sizeof(run_infer_param_));
    LogDebug("LLM_DBG RkllmSession::Shutdown done");
}

// rkllm_is_running 返回 0 表示仍在运行。
bool RkllmSession::IsRunning() const {
    if (!handle_) {
        return false;
    }
    return rkllm_is_running(handle_) == 0;
}

// 绑定可选回复累积缓冲（与终端 AI> 同源）。
void RkllmSession::SetReplyAccumulator(std::string* accumulator) {
    std::lock_guard<std::mutex> lock(reply_mutex_);
    reply_accumulator_ = accumulator;
}

// 取出并清空累积回复。
std::string RkllmSession::TakeReplyAccumulator() {
    std::lock_guard<std::mutex> lock(reply_mutex_);
    std::string out;
    if (reply_accumulator_) {
        out.swap(*reply_accumulator_);
    }
    return out;
}

// librkllmrt 回调：NORMAL/FINISH 直写 stdout；状态经 chunk_fn_ 通知上层。
void RkllmSession::StaticCallback(RKLLMResult* result, void* userdata, LLMCallState state) {
    RkllmSession* self = static_cast<RkllmSession*>(userdata);
    if (!self || self->magic_ != kMagic) {
        self = g_active_session;
    }
    if (!self || self->magic_ != kMagic) {
        return;
    }

    if (state == RKLLM_RUN_FINISH) {
        SessionStdoutWrite("\n");
        EndLlmStdoutStream();
    } else if (state == RKLLM_RUN_ERROR) {
        SessionStdoutWrite("\\run error\n");
        EndLlmStdoutStream();
    } else if (state == RKLLM_RUN_NORMAL && result && result->text) {
        SessionStdoutWrite(result->text);
        std::lock_guard<std::mutex> lock(self->reply_mutex_);
        if (self->reply_accumulator_) {
            *self->reply_accumulator_ += result->text;
        }
    }

    if (!self->chunk_fn_) {
        return;
    }
    const char* chunk = (result && result->text) ? result->text : "";
    self->chunk_fn_(chunk, state, self->chunk_user_data_);
}
