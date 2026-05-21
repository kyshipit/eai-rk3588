/*
 * adapters/llm/llm_worker.cpp
 */
#include "llm_worker.h"

#include "platform/logging.h"

LlmWorker::LlmWorker() = default;

LlmWorker::~LlmWorker() {
    Shutdown();
}

int LlmWorker::Init(const std::string& model_path, int max_new_tokens, int max_context_len) {
    return session_.Init(
        model_path, max_new_tokens, max_context_len,
        [this](const char* text_chunk, LLMCallState state) { OnLlmChunk(text_chunk, state); });
}

void LlmWorker::SetBannerCallback(BannerCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    banner_cb_ = std::move(cb);
}

void LlmWorker::RequestGreeting(const std::string& user_prompt) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (infer_busy_ || !session_.IsInitialized()) {
            return;
        }
        infer_busy_ = true;
        pending_text_.clear();
    }
    const int ret = session_.RunPromptAsync(user_prompt);
    if (ret != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        infer_busy_ = false;
        LogWarn("LlmWorker: rkllm_run_async failed (%d)", ret);
    } else {
        LogInfo("LlmWorker: greeting inference started");
    }
}

void LlmWorker::Shutdown() {
    session_.Shutdown();
    std::lock_guard<std::mutex> lock(mutex_);
    infer_busy_ = false;
    pending_text_.clear();
}

void LlmWorker::OnLlmChunk(const char* text_chunk, LLMCallState state) {
    BannerCallback cb;
    std::string final_line;
    bool done = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state == RKLLM_RUN_NORMAL && text_chunk && text_chunk[0] != '\0') {
            pending_text_ += text_chunk;
        } else if (state == RKLLM_RUN_FINISH) {
            final_line = pending_text_;
            pending_text_.clear();
            infer_busy_ = false;
            done = !final_line.empty();
            cb = banner_cb_;
        } else if (state == RKLLM_RUN_ERROR) {
            LogWarn("LlmWorker: RKLLM_RUN_ERROR");
            pending_text_.clear();
            infer_busy_ = false;
        }
    }

    if (done && cb) {
        cb(final_line);
        LogInfo("LlmWorker: greeting ready (%zu chars)", final_line.size());
    }
}
