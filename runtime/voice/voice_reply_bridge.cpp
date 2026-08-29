// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 KY (kyshipit)

/*
 * voice/voice_reply_bridge.cpp — LLM chunk 到 TTS 正式回答的旁路装配。
 */
#include "voice_reply_bridge.h"

#include "tts_worker.h"

namespace {

constexpr size_t kFormalPrefetchMinChars = 48;

// 判断文本是否含句末标点，用于预取入队时机。
bool HasSentenceEndPunctuation(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = text[i];
        if (c == '!' || c == '?' || c == ';' || c == '.') {
            return true;
        }
        if (i + 2 < text.size() && c == 0xEF && static_cast<unsigned char>(text[i + 1]) == 0xBC) {
            const unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
            if (c2 == 0x8E || c2 == 0x81 || c2 == 0x9F || c2 == 0x9B) {
                return true;
            }
        }
        if (i + 2 < text.size() && c == 0xE3 && static_cast<unsigned char>(text[i + 1]) == 0x80) {
            const unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
            if (c2 == 0x82 || c2 == 0x81) {
                return true;
            }
        }
        if ((c & 0x80) == 0) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            i += 2;
            continue;
        }
        if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
            i += 3;
            continue;
        }
        if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
            i += 4;
            continue;
        }
        ++i;
    }
    return false;
}

}  // namespace

// 绑定 TTS 工作线程；解绑时清空旁路状态。
void VoiceReplyBridge::SetTtsWorker(TtsWorker* tts) {
    std::lock_guard<std::mutex> lock(mutex_);
    tts_ = tts;
    if (!tts_) {
        events_.clear();
        ingress_.Reset();
        planner_.Reset();
        ResetFormalPipelineUnlocked();
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
        ResetFormalPipelineUnlocked();
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
        ResetFormalPipelineUnlocked();
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
    ResetFormalPipelineUnlocked();
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

// 主线程：首句见句末即送 TTS；后续攒字预取，FINISH  flush 尾段。
void VoiceReplyBridge::Poll() {
    TtsWorker* tts = nullptr;
    std::vector<std::string> segments;
    bool run_finished = false;
    std::string immediate;
    std::string prefetch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || !tts_ || events_.empty()) {
            return;
        }
        DrainDeferredTtsEventsUnlocked(segments, run_finished);
        AccumulateFormalSegmentsUnlocked(segments, run_finished, immediate, prefetch);
        tts = tts_;
    }
    if (!tts) {
        return;
    }
    if (!immediate.empty()) {
        tts->EnqueueFormalAnswer(immediate);
    }
    if (!prefetch.empty()) {
        tts->EnqueueFormalAnswer(prefetch);
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
        ResetFormalPipelineUnlocked();
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
    ResetFormalPipelineUnlocked();
    active_run_session_id_ = desired_session_id_;
}

// 当前 run 代际是否仍为用户期望的最新代际。
bool VoiceReplyBridge::IsCurrentRunLiveUnlocked() const {
    return active_run_session_id_ == desired_session_id_;
}

// 复位本轮正式回答攒字状态。
void VoiceReplyBridge::ResetFormalPipelineUnlocked() {
    deferred_formal_.clear();
    formal_pipeline_started_ = false;
}

// 首句立刻入队；后续攒至句界/阈值预取，FINISH 时 flush 剩余。
void VoiceReplyBridge::AccumulateFormalSegmentsUnlocked(const std::vector<std::string>& segments,
                                                        bool run_finished,
                                                        std::string& immediate_out,
                                                        std::string& prefetch_out) {
    std::string batch;
    for (const auto& segment : segments) {
        if (!segment.empty()) {
            batch += segment;
        }
    }
    if (batch.empty() && !run_finished) {
        return;
    }

    if (!formal_pipeline_started_) {
        if (!batch.empty()) {
            immediate_out = std::move(batch);
            formal_pipeline_started_ = true;
        }
        if (run_finished) {
            ResetFormalPipelineUnlocked();
        }
        return;
    }

    deferred_formal_ += batch;

    if (run_finished) {
        if (!deferred_formal_.empty()) {
            prefetch_out = std::move(deferred_formal_);
            deferred_formal_.clear();
        }
        ResetFormalPipelineUnlocked();
        return;
    }

    if (deferred_formal_.empty()) {
        return;
    }
    const size_t deferred_chars = utf8_strlen(deferred_formal_);
    if (HasSentenceEndPunctuation(deferred_formal_) ||
        deferred_chars >= kFormalPrefetchMinChars) {
        prefetch_out = std::move(deferred_formal_);
        deferred_formal_.clear();
    }
}

// 锁内消费事件队列，经 Ingress/Planner 产出待播报片段。
void VoiceReplyBridge::DrainDeferredTtsEventsUnlocked(std::vector<std::string>& segments_out,
                                                      bool& run_finished_out) {
    run_finished_out = false;
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
            run_finished_out = true;
            continue;
        }
        if (event.state == RKLLM_RUN_ERROR) {
            ingress_.Reset();
            planner_.Reset();
            ResetFormalPipelineUnlocked();
            run_finished_out = true;
        }
    }
}
