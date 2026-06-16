/*
 * voice/voice_reply_bridge.cpp — LLM chunk 到 TTS 正式回答的旁路装配。
 */
#include "voice_reply_bridge.h"

#include "tts_worker.h"

// 绑定 TTS 工作线程；解绑时清空旁路状态。
void VoiceReplyBridge::SetTtsWorker(TtsWorker* tts) {
    std::lock_guard<std::mutex> lock(mutex_);
    tts_ = tts;
    if (!tts_) {
        events_.clear();
        ingress_.Reset();
        planner_.Reset();
    }
}

// 写入 Planner 切分阈值与短答策略。
void VoiceReplyBridge::ConfigurePlanner(const TtsPlannerConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    planner_.Configure(cfg);
}

// 关闭时丢弃已排队事件并重置 Ingress/Planner。
void VoiceReplyBridge::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    if (!enabled_) {
        events_.clear();
        ingress_.Reset();
        planner_.Reset();
    }
}

// 用户新输入抢占：Cancel 旧音、升代际、清旁路状态。
void VoiceReplyBridge::BeginTurn() {
    TtsWorker* tts = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || !tts_) {
            return;
        }
        tts = tts_;
        ++desired_session_id_;
        if (desired_session_id_ == 0) {
            desired_session_id_ = 1;
        }
        events_.clear();
        ingress_.Reset();
        planner_.Reset();
    }
    tts->Cancel();
}

// 本轮 rkllm_run 开始时锁定代际，丢弃上一轮迟到的回调。
void VoiceReplyBridge::OnRunStarted() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_run_session_id_ = desired_session_id_;
    events_.clear();
    ingress_.Reset();
    planner_.Reset();
}

// 回调线程仅入队；NORMAL/FINISH/ERROR 由主线程 Poll 消费。
void VoiceReplyBridge::OnLlmChunk(const char* text_chunk, LLMCallState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || !tts_) {
        return;
    }
    if (!IsCurrentRunLiveUnlocked()) {
        return;
    }
    if (state == RKLLM_RUN_NORMAL) {
        if (!text_chunk || text_chunk[0] == '\0') {
            return;
        }
        TtsEvent event;
        event.session_id = active_run_session_id_;
        event.state = state;
        event.chunk = text_chunk;
        events_.push_back(std::move(event));
        return;
    }
    if (state == RKLLM_RUN_FINISH || state == RKLLM_RUN_ERROR) {
        TtsEvent event;
        event.session_id = active_run_session_id_;
        event.state = state;
        events_.push_back(std::move(event));
    }
}

// 主线程：Ingress/Planner 规划后合并本批片段再 Enqueue，减少「一段一 decoder」与播放断粮。
void VoiceReplyBridge::Poll() {
    TtsWorker* tts = nullptr;
    std::vector<std::string> segments;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || !tts_ || events_.empty()) {
            return;
        }
        DrainDeferredTtsEventsUnlocked(segments);
        tts = tts_;
    }
    if (segments.empty() || !tts) {
        return;
    }
    std::string merged;
    for (const auto& segment : segments) {
        if (!segment.empty()) {
            merged += segment;
        }
    }
    if (!merged.empty()) {
        tts->EnqueueFormalAnswer(merged);
    }
}

// 进程退出或 Abort：Cancel 播报并清空队列。
void VoiceReplyBridge::Cancel() {
    TtsWorker* tts = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tts = tts_;
        events_.clear();
        ingress_.Reset();
        planner_.Reset();
    }
    if (tts) {
        tts->Cancel();
    }
}

// Shutdown 时复位代际与旁路状态。
void VoiceReplyBridge::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();
    ingress_.Reset();
    planner_.Reset();
    active_run_session_id_ = desired_session_id_;
}

// 当前 run 代际是否仍为用户期望的最新代际。
bool VoiceReplyBridge::IsCurrentRunLiveUnlocked() const {
    return active_run_session_id_ == desired_session_id_;
}

// 锁内消费事件队列，经 Ingress/Planner 产出待播报片段。
void VoiceReplyBridge::DrainDeferredTtsEventsUnlocked(std::vector<std::string>& segments_out) {
    while (!events_.empty()) {
        TtsEvent event = std::move(events_.front());
        events_.pop_front();
        if (event.session_id != desired_session_id_) {
            continue;
        }
        if (event.state == RKLLM_RUN_NORMAL) {
            if (!event.chunk.empty()) {
                std::string visible_delta;
                ingress_.Feed(event.chunk.c_str(), visible_delta);
                if (!visible_delta.empty()) {
                    planner_.Feed(visible_delta, segments_out);
                }
            }
            continue;
        }
        if (event.state == RKLLM_RUN_FINISH) {
            std::string visible_delta;
            ingress_.Flush(visible_delta);
            if (!visible_delta.empty()) {
                planner_.Feed(visible_delta, segments_out);
            }
            planner_.Flush(segments_out);
            continue;
        }
        if (event.state == RKLLM_RUN_ERROR) {
            ingress_.Reset();
            planner_.Reset();
        }
    }
}
