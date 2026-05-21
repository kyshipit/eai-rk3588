/*
 * adapters/llm/rkllm_session.cpp
 */
#include "rkllm_session.h"

#include <cstring>

namespace {
constexpr const char* kPromptPrefix =
    "<｜begin▁of▁sentence｜><｜User｜>";
constexpr const char* kPromptPostfix = "<｜Assistant｜>";
}  // namespace

RkllmSession::RkllmSession() = default;

RkllmSession::~RkllmSession() {
    Shutdown();
}

int RkllmSession::Init(const std::string& model_path, int max_new_tokens, int max_context_len,
                         ResultCallback callback) {
    Shutdown();
    callback_ = std::move(callback);

    RKLLMParam param = rkllm_createDefaultParam();
    param.model_path = model_path.c_str();
    param.top_k = 1;
    param.top_p = 0.95f;
    param.temperature = 0.8f;
    param.repeat_penalty = 1.1f;
    param.frequency_penalty = 0.0f;
    param.presence_penalty = 0.0f;
    param.max_new_tokens = max_new_tokens;
    param.max_context_len = max_context_len;
    param.skip_special_token = true;
    param.is_async = true;
    param.extend_param.base_domain_id = 0;

    return rkllm_init(&handle_, &param, &RkllmSession::StaticCallback);
}

int RkllmSession::RunPromptAsync(const std::string& user_text) {
    if (!handle_) {
        return -1;
    }
    prompt_buffer_ = std::string(kPromptPrefix) + user_text + kPromptPostfix;

    RKLLMInput input;
    std::memset(&input, 0, sizeof(input));
    input.input_type = RKLLM_INPUT_PROMPT;
    input.prompt_input = prompt_buffer_.data();

    RKLLMInferParam infer_params;
    std::memset(&infer_params, 0, sizeof(infer_params));
    infer_params.mode = RKLLM_INFER_GENERATE;

    return rkllm_run_async(handle_, &input, &infer_params, this);
}

int RkllmSession::Abort() {
    if (!handle_) {
        return -1;
    }
    return rkllm_abort(handle_);
}

void RkllmSession::Shutdown() {
    if (handle_) {
        Abort();
        LLMHandle tmp = handle_;
        handle_ = nullptr;
        rkllm_destroy(tmp);
    }
    callback_ = nullptr;
}

bool RkllmSession::IsRunning() const {
    if (!handle_) {
        return false;
    }
    return rkllm_is_running(handle_) == 0;
}

void RkllmSession::StaticCallback(RKLLMResult* result, void* userdata, LLMCallState state) {
    auto* self = static_cast<RkllmSession*>(userdata);
    if (!self || !self->callback_) {
        return;
    }
    const char* chunk = (result && result->text) ? result->text : "";
    self->callback_(chunk, state);
}
