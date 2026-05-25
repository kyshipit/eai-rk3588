/*
 * platform/llm_greeting.h — 人脸门控与会话状态机（Locked/Arming/Active/Grace）。
 */
#pragma once

#include <mutex>
#include <chrono>
#include <string>
#include "adapter_signals.h"
#include "adapters/llm/llm_worker.h"

class LlmGreeting {
public:
    // 重置门控与计数状态。
    void Reset();
    // 绑定 LLM worker（并注册输出回调）。
    void SetLlmWorker(LlmWorker* worker);
    // 设置自动问候文本（严格按配置覆盖，允许空串关闭自动问候）。
    void SetAutoGreetingText(const std::string& text);
    // 设置稳定人脸触发阈值。
    void SetTriggerThreshold(int frames);
    // 设置人脸缺失关门阈值。
    void SetFaceAbsentThreshold(int frames);
    // 设置宽限期时长（毫秒）：进入 Grace 后超时才真正锁定。
    void SetGraceTimeoutMs(int timeout_ms);
    // 设置会话空闲超时（毫秒）：长时间无输入/无输出自动回锁定。
    void SetIdleTimeoutMs(int timeout_ms);
    // 设置 SCRFD 激活时是否预加载模型。
    void SetPreloadOnScrfd(bool preload);

    // 每帧更新门控状态。
    void Update(const AdapterSignals& signals, bool scrfd_slot_active);
    // 每帧轮询 worker 延迟任务。
    void PollDeferred();

    // 麦克风/按键等：gate 关闭时拒绝。
    bool SubmitUserPrompt(const std::string& text);

    // 进程退出或 Pipeline::Stop 时中断正在进行的生成。
    void AbortActiveGeneration();

    // 统一处理 AI 文本输出（支持单行连续流式）。
    void SetBannerLine(const std::string& line, LlmPromptSource src, bool is_final = true);

private:
    enum class SessionState {
        Locked,
        Arming,
        Active,
        Grace
    };

    static const char* StateName(SessionState state);
    void TryPreload();
    void TryAutoPromptOnStableFace(const AdapterSignals& signals);
    static const char* SourceName(LlmPromptSource src);

    LlmWorker* worker_ = nullptr;
    std::string auto_greeting_text_;
    int face_stable_count_ = 0;
    int face_absent_count_ = 0;
    int trigger_threshold_ = 5;
    int face_absent_threshold_ = 10;
    int grace_timeout_ms_ = 5000;
    int idle_timeout_ms_ = 60000;
    bool preload_on_scrfd_ = true;
    bool prompt_gate_open_ = false;
    bool gate_reject_notified_ = false;
    bool auto_prompt_sent_for_visit_ = false;
    bool had_face_leave_since_boot_ = false;
    bool scrfd_was_active_ = false;
    std::mutex ai_stream_mutex_;
    bool ai_stream_open_ = false;
    LlmPromptSource ai_stream_src_ = LlmPromptSource::FaceAppear;
    std::string ai_stream_text_;
    SessionState state_ = SessionState::Locked;
    std::chrono::steady_clock::time_point grace_deadline_;
    std::chrono::steady_clock::time_point last_activity_ts_;
};
