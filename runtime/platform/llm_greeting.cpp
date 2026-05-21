/*
 * platform/llm_greeting.cpp
 */
#include "llm_greeting.h"

#include "adapters/llm/llm_worker.h"
#include "logging.h"

void LlmGreeting::Reset() {
    last_face_detected_ = false;
    face_stable_count_ = 0;
    banner_line_.clear();
    greeted_this_session_ = false;
}

void LlmGreeting::SetLlmWorker(LlmWorker* worker) {
    worker_ = worker;
    if (worker_) {
        worker_->SetBannerCallback([this](const std::string& line) { SetBannerLine(line); });
    }
}

void LlmGreeting::SetGreetingPrompt(const std::string& prompt) {
    if (!prompt.empty()) {
        greeting_prompt_ = prompt;
    }
}

void LlmGreeting::SetTriggerThreshold(int frames) {
    if (frames > 0) {
        trigger_threshold_ = frames;
    }
}

void LlmGreeting::SetBannerLine(const std::string& line) {
    banner_line_ = line;
}

void LlmGreeting::Update(const AdapterSignals& signals, bool scrfd_slot_active) {
    if (!scrfd_slot_active) {
        face_stable_count_ = 0;
        last_face_detected_ = false;
        return;
    }
    if (signals.face_detected) {
        face_stable_count_++;
    } else {
        face_stable_count_ = 0;
    }
    if (!greeted_this_session_ && face_stable_count_ >= trigger_threshold_ &&
        !last_face_detected_ && signals.face_detected) {
        greeted_this_session_ = true;
        if (worker_ && worker_->IsReady()) {
            worker_->RequestGreeting(greeting_prompt_);
            LogInfo("LlmGreeting: face stable -> RKLLM greeting requested");
        } else {
            banner_line_ = "Hello! (LLM disabled or not ready)";
            LogInfo("LlmGreeting: face stable -> placeholder (LLM not ready)");
        }
    }
    last_face_detected_ = signals.face_detected;
}
