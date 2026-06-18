/*
 * voice/voice_reply_bridge.h
 *
 * LLM 流式 chunk → TtsIngress/Planner → TtsWorker 装配层；与 LlmWorker 解耦。
 */
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "rkllm.h"
#include "split.hpp"
#include "tts_ingress.h"
#include "tts_planner.h"

class TtsWorker;

class VoiceReplyBridge {
public:
    // 绑定播报器；空指针时仅丢弃事件。
    void SetTtsWorker(TtsWorker* tts);
    // 配置正式回答规划参数。
    void ConfigurePlanner(const TtsPlannerConfig& cfg);
    // 是否处理 LLM chunk 并下发 FormalAnswer。
    void SetEnabled(bool enabled);

    // 新 YOU> 或抢占：Cancel TTS、递增代际、清空 Ingress/Planner 与事件队列。
    void BeginTurn();
    // rkllm_run 开始时标记当前 run 代际（回调线程只接受该代际事件）。
    void OnRunStarted();
    // RKLLM 回调线程入队；禁止在回调内做 Planner/Enqueue。
    void OnLlmChunk(const char* text_chunk, LLMCallState state);
    // 主线程消费事件并 EnqueueFormalAnswer。
    void Poll();
    // 退出/中断：Cancel 播报并丢弃排队事件。
    void Cancel();
    // Shutdown 时清空状态。
    void Reset();

private:
    struct TtsEvent {
        uint64_t session_id = 0;
        LLMCallState state = RKLLM_RUN_NORMAL;
        std::string chunk;
    };

    void DrainDeferredTtsEventsUnlocked(std::vector<std::string>& segments_out, bool& run_finished_out);
    bool IsCurrentRunLiveUnlocked() const;
    // 复位本轮正式回答的攒字/首句开工状态。
    void ResetFormalPipelineUnlocked();
    // 将 Planner 本批片段并入攒字缓冲，产出应立即送 TTS 与可预取块。
    void AccumulateFormalSegmentsUnlocked(const std::vector<std::string>& segments, bool run_finished,
                                          std::string& immediate_out, std::string& prefetch_out);

    TtsWorker* tts_ = nullptr;
    bool enabled_ = false;
    TtsIngress ingress_;
    TtsPlanner planner_;
    std::deque<TtsEvent> events_;
    uint64_t desired_session_id_ = 1;
    uint64_t active_run_session_id_ = 1;
    // 首句已送 TTS 后，后续片段先攒在此，达句界或阈值再预取入队。
    std::string deferred_formal_;
    bool formal_pipeline_started_ = false;
    mutable std::mutex mutex_;
};
