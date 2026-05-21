/*
 * platform/llm_greeting.h — 异步逻辑槽：人脸稳定后触发问候（经 adapters/llm/LlmWorker）。
 */
#pragma once

#include <string>
#include "adapter_signals.h"

class LlmWorker;

class LlmGreeting {
public:
    void Reset();
    void SetLlmWorker(LlmWorker* worker);
    void SetGreetingPrompt(const std::string& prompt);
    void SetTriggerThreshold(int frames);
    void Update(const AdapterSignals& signals, bool scrfd_slot_active);
    void SetBannerLine(const std::string& line);
    const std::string& GetBannerLine() const { return banner_line_; }
    bool HasBanner() const { return !banner_line_.empty(); }

private:
    LlmWorker* worker_ = nullptr;
    std::string greeting_prompt_ =
        "请用一句简短、自然的中文向镜头前的人问好，不要超过二十个字。";
    bool last_face_detected_ = false;
    int face_stable_count_ = 0;
    int trigger_threshold_ = 10;
    std::string banner_line_;
    bool greeted_this_session_ = false;
};
