/*
 * adapters/tts/tts_worker.cpp
 */
#include "tts_worker.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "audio_player.h"
#include "melotts_process.h"
#include "platform/logging.h"
#include "tts_text_sanitizer.h"

// 延迟加载 RKNN，由 Configure / RequestInitializeAsync 触发。
TtsWorker::TtsWorker() = default;

// 停止后台线程并释放 RKNN。
TtsWorker::~TtsWorker() {
    Shutdown();
}

// 保存合成参数，不立即加载模型。
void TtsWorker::Configure(const MeloTtsConfig& cfg, int max_speak_chars) {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_ = cfg;
    max_speak_chars_ = max_speak_chars;
    configured_ = true;
    if (init_state_ == InitState::Failed) {
        init_state_ = InitState::Uninitialized;
    }
}

// 重置指定代际的统计状态，等待首条文本触发计时起点。
void TtsWorker::ResetGenerationStatsUnlocked(uint64_t generation) {
    stats_generation_ = generation;
    stats_started_ = false;
    stats_finalized_ = false;
    first_pcm_enqueued_ = false;
    first_pcm_played_ = false;
    underrun_latched_ = false;
    underrun_count_ = 0;
    first_text_enqueue_tp_ = std::chrono::steady_clock::now();
}

// 在当前代际结束时输出统计摘要，便于对比首响与断粮改造效果。
void TtsWorker::FinalizeGenerationStatsUnlocked() {
    if (!stats_started_ || stats_finalized_) {
        return;
    }
    stats_finalized_ = true;
    LogInfo("TtsWorker: generation=%llu summary first_pcm_enqueued=%d first_pcm_played=%d underrun=%u",
            static_cast<unsigned long long>(stats_generation_),
            first_pcm_enqueued_ ? 1 : 0,
            first_pcm_played_ ? 1 : 0,
            underrun_count_);
}

// 记录一次连续断粮窗口（队列见底且仍在播报阶段），同一窗口只计数一次。
void TtsWorker::MarkUnderrunUnlocked() {
    if (!stats_started_ || stats_finalized_ || underrun_latched_) {
        return;
    }
    underrun_latched_ = true;
    ++underrun_count_;
    LogWarn("TtsWorker: generation=%llu pcm underrun #%u",
            static_cast<unsigned long long>(stats_generation_),
            underrun_count_);
}

// 异步加载 Lexicon 与 RKNN。
void TtsWorker::RequestInitializeAsync() {
    MeloTtsConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_ || init_state_ == InitState::Ready ||
            init_state_ == InitState::Initializing) {
            return;
        }
        cfg = cfg_;
        init_state_ = InitState::Initializing;
    }
    LogInfo("TtsWorker: async init start...");
    init_future_ = std::async(std::launch::async, [this, cfg]() {
        return session_.Init(cfg);
    });
}

// 主线程轮询 init 结果。
void TtsWorker::PollInitState() {
    std::future<bool> done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (init_state_ != InitState::Initializing || !init_future_.valid()) {
            return;
        }
        if (init_future_.wait_for(std::chrono::milliseconds(0)) !=
            std::future_status::ready) {
            return;
        }
        done = std::move(init_future_);
    }
    const bool ok = done.get();
    bool notify_workers = false;
    if (ok) {
        BuildFastAckCacheIfNeeded();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        init_state_ = ok ? InitState::Ready : InitState::Failed;
        if (ok) {
            LogInfo("TtsWorker: init ok");
            if (!synth_thread_.joinable()) {
                stop_ = false;
                synth_thread_ = std::thread(&TtsWorker::SynthesizeLoop, this);
            }
            if (!play_thread_.joinable()) {
                play_thread_ = std::thread(&TtsWorker::PlaybackLoop, this);
            }
            notify_workers = !text_queue_.empty() || !pcm_queue_.empty();
        } else {
            LogWarn("TtsWorker: init failed");
            FinalizeGenerationStatsUnlocked();
            text_queue_.clear();
            pcm_queue_.clear();
            synth_busy_ = false;
            protect_latched_ = false;
            ResetGenerationStatsUnlocked(generation_);
        }
    }
    if (notify_workers) {
        cv_.notify_all();
    }
}

// 清洗后入队；未 ready 时触发 init，保留排队文本等待模型就绪。
void TtsWorker::EnqueueCleaned(std::string cleaned, TextJobKind kind) {
    if (cleaned.empty()) {
        return;
    }
    bool need_init = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (init_state_ == InitState::Failed) {
            return;
        }
        TextJob job;
        job.kind = kind;
        job.generation = generation_;
        job.text = std::move(cleaned);
        if (stats_generation_ != generation_) {
            ResetGenerationStatsUnlocked(generation_);
        }
        if (!stats_started_) {
            stats_started_ = true;
            first_text_enqueue_tp_ = std::chrono::steady_clock::now();
        }
        text_queue_.push_back(std::move(job));
        need_init = (init_state_ == InitState::Uninitialized);
    }
    if (need_init) {
        RequestInitializeAsync();
    }
    cv_.notify_all();
}

// 将一段文本清洗后追加到待合成队列（问候等整段播报，立即开播）。
void TtsWorker::PlayText(const std::string& text) {
    EnqueueCleaned(TtsTextSanitizer::Sanitize(text, max_speak_chars_), TextJobKind::Static);
}

// 将流式正式回答片段清洗后追加到待合成队列。
void TtsWorker::EnqueueFormalAnswer(const std::string& text) {
    EnqueueCleaned(TtsTextSanitizer::Sanitize(text, max_speak_chars_), TextJobKind::FormalAnswer);
}

// 兼容旧接口。
void TtsWorker::EnqueueSentence(const std::string& text) {
    EnqueueFormalAnswer(text);
}

// 配置短反馈文案。
void TtsWorker::ConfigureFastAck(bool enabled, const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    fast_ack_enabled_ = enabled;
    fast_ack_text_ = text;
    fast_ack_pcm_.clear();
    fast_ack_cache_built_ = false;
}

// TTS ready 后预合成短反馈 PCM。
void TtsWorker::BuildFastAckCacheIfNeeded() {
    std::string ack_text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!fast_ack_enabled_ || fast_ack_text_.empty() || fast_ack_cache_built_) {
            return;
        }
        if (init_state_ != InitState::Ready || !session_.IsReady()) {
            return;
        }
        ack_text = fast_ack_text_;
    }
    std::vector<float> pcm;
    if (!session_.SynthesizeTextStreaming(ack_text, [&pcm](std::vector<float>&& chunk) {
            if (!chunk.empty()) {
                pcm.insert(pcm.end(), chunk.begin(), chunk.end());
            }
            return true;
        })) {
        LogWarn("TtsWorker: fast ack cache synthesize failed");
        return;
    }
    if (pcm.empty()) {
        LogWarn("TtsWorker: fast ack cache empty pcm");
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    fast_ack_pcm_ = std::move(pcm);
    fast_ack_cache_built_ = true;
    LogInfo("TtsWorker: fast ack cache ready (%zu samples)", fast_ack_pcm_.size());
}

// 播放预缓存短反馈 PCM，不等待 min_start_pcm_chunks。
void TtsWorker::PlayFastAck() {
    if (!fast_ack_enabled_) {
        return;
    }
    BuildFastAckCacheIfNeeded();
    std::vector<float> pcm_copy;
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fast_ack_pcm_.empty()) {
            LogWarn("TtsWorker: PlayFastAck skipped (cache not ready)");
            return;
        }
        pcm_copy = fast_ack_pcm_;
        gen = generation_;
        if (stats_generation_ != generation_) {
            ResetGenerationStatsUnlocked(generation_);
        }
        if (!stats_started_) {
            stats_started_ = true;
            first_text_enqueue_tp_ = std::chrono::steady_clock::now();
        }
        PcmJob job;
        job.kind = PcmJobKind::FastAck;
        job.generation = gen;
        job.pcm = std::move(pcm_copy);
        pcm_queue_.push_back(std::move(job));
        RefreshProtectionLatchUnlocked();
    }
    cv_.notify_all();
}

// 停止播放并清空待合成/待播放；代际号自增以丢弃旧会话在途任务（不杀 gst，保管道连续）。
void TtsWorker::Cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        FinalizeGenerationStatsUnlocked();
        ++generation_;
        text_queue_.clear();
        pcm_queue_.clear();
        synth_busy_ = false;
        protect_latched_ = false;
        formal_playback_started_ = false;
        ResetGenerationStatsUnlocked(generation_);
    }
    cv_.notify_all();
}

// 退出 worker 线程并 shutdown session。
void TtsWorker::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        FinalizeGenerationStatsUnlocked();
        ++generation_;
        text_queue_.clear();
        pcm_queue_.clear();
        synth_busy_ = false;
        protect_latched_ = false;
        formal_playback_started_ = false;
        ResetGenerationStatsUnlocked(generation_);
    }
    AudioPlayer player;
    player.Stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    JoinWorkerThreads();
    if (init_future_.valid()) {
        init_future_.wait();
    }
    session_.Shutdown();
    std::lock_guard<std::mutex> lock(mutex_);
    init_state_ = InitState::Uninitialized;
}

// 查询是否 ready。
bool TtsWorker::IsReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsReadyUnlocked();
}

// 查询是否正在初始化。
bool TtsWorker::IsInitializing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return init_state_ == InitState::Initializing;
}

// 返回 max_speak_chars 配置。
int TtsWorker::MaxSpeakChars() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_speak_chars_;
}

// 配置播放保护阈值：片段数低于 low 触发保护，高于 high 解除保护。
void TtsWorker::SetPlaybackProtectionThresholds(size_t low_chunks, size_t high_chunks) {
    std::lock_guard<std::mutex> lock(mutex_);
    protect_low_chunks_ = std::max<size_t>(1, low_chunks);
    protect_high_chunks_ = std::max(protect_low_chunks_ + 1, high_chunks);
    RefreshProtectionLatchUnlocked();
}

// 配置首次开播前需要积攒的 PCM 片段数，默认 2 段以换取连续性。
void TtsWorker::SetMinStartPcmChunks(size_t chunks) {
    std::lock_guard<std::mutex> lock(mutex_);
    min_start_pcm_chunks_ = std::max<size_t>(1, chunks);
}

// 查询是否应进入播放保护窗口；最小版本中整轮播报活跃期都保护。
bool TtsWorker::NeedPlaybackProtection() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsPlaybackActiveUnlocked();
}

// 查询是否有在途播报（含合成线程执行中）。
bool TtsWorker::IsPlaybackActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsPlaybackActiveUnlocked();
}

// 锁内 ready 判定。
bool TtsWorker::IsReadyUnlocked() const {
    return init_state_ == InitState::Ready;
}

// 锁内判断：存在文本队列、PCM 队列或当前正在合成即视为活跃播报。
bool TtsWorker::IsPlaybackActiveUnlocked() const {
    return synth_busy_ || !text_queue_.empty() || !pcm_queue_.empty();
}

// 锁内刷新保护锁存：仅保留水位日志语义，保护窗口由整轮活跃期决定。
void TtsWorker::RefreshProtectionLatchUnlocked() {
    if (!IsPlaybackActiveUnlocked()) {
        FinalizeGenerationStatsUnlocked();
        protect_latched_ = false;
        underrun_latched_ = false;
        return;
    }
    const size_t depth = pcm_queue_.size();
    if (depth > 0) {
        underrun_latched_ = false;
    }
    if (depth <= protect_low_chunks_) {
        protect_latched_ = true;
        return;
    }
    protect_latched_ = depth <= protect_low_chunks_;
}

// 等待后台线程结束。
void TtsWorker::JoinWorkerThreads() {
    if (synth_thread_.joinable()) {
        synth_thread_.join();
    }
    if (play_thread_.joinable()) {
        play_thread_.join();
    }
}

// 合并同一代际下队列中已到达的连续文本块，减少 RKNN 往返与播放断粮。
std::string TtsWorker::CoalescePendingTextLocked(TextJob first) {
    constexpr size_t kMinChunkBytes = 4;
    constexpr size_t kMaxChunkBytes = 20;
    std::string merged = std::move(first.text);
    while (!text_queue_.empty() && text_queue_.front().generation == first.generation) {
        if (merged.size() >= kMinChunkBytes) {
            break;
        }
        const std::string& next = text_queue_.front().text;
        if (merged.size() + next.size() > kMaxChunkBytes) {
            break;
        }
        merged += text_queue_.front().text;
        text_queue_.pop_front();
    }
    return merged;
}

// 将合成出来的单个 PCM 片段压入播放队列；代际不匹配时拒收并终止本轮。
bool TtsWorker::PushPcmChunk(uint64_t generation, std::vector<float> pcm, PcmJobKind kind) {
    if (pcm.empty()) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_ || generation != generation_) {
            return false;
        }
        PcmJob pcm_job;
        pcm_job.kind = kind;
        pcm_job.generation = generation;
        pcm_job.pcm = std::move(pcm);
        pcm_queue_.push_back(std::move(pcm_job));
        if (stats_generation_ == generation && stats_started_ && !first_pcm_enqueued_) {
            first_pcm_enqueued_ = true;
            const auto delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - first_text_enqueue_tp_)
                                      .count();
            LogInfo("TtsWorker: generation=%llu first pcm enqueued in %lld ms",
                    static_cast<unsigned long long>(generation),
                    static_cast<long long>(delay_ms));
        }
        RefreshProtectionLatchUnlocked();
    }
    cv_.notify_all();
    return true;
}

// 合成线程：FIFO 取文本并产出 PCM，同轮多块合并后再推理。
void TtsWorker::SynthesizeLoop() {
    while (true) {
        TextJob job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return stop_ || (IsReadyUnlocked() && !text_queue_.empty());
            });
            if (stop_) {
                break;
            }
            job = std::move(text_queue_.front());
            text_queue_.pop_front();
            if (job.kind != TextJobKind::FormalAnswer) {
                job.text = CoalescePendingTextLocked(std::move(job));
            }
            synth_busy_ = true;
            RefreshProtectionLatchUnlocked();
        }
        if (job.text.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            synth_busy_ = false;
            RefreshProtectionLatchUnlocked();
            continue;
        }
        const bool emitted = session_.SynthesizeTextStreaming(
            job.text, [this, generation = job.generation, pcm_kind = job.kind](
                          std::vector<float>&& pcm_chunk) {
                const PcmJobKind kind =
                    (pcm_kind == TextJobKind::Static) ? PcmJobKind::Static : PcmJobKind::Formal;
                return PushPcmChunk(generation, std::move(pcm_chunk), kind);
            });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            synth_busy_ = false;
            RefreshProtectionLatchUnlocked();
        }
        if (!emitted && job.kind == TextJobKind::FormalAnswer) {
            LogWarn("TtsWorker: synthesize empty text=\"%.48s%s\"",
                    job.text.c_str(), job.text.size() > 48 ? "..." : "");
        }
    }
}

// 播放线程：FIFO 取 PCM 连续播放。
void TtsWorker::PlaybackLoop() {
    AudioPlayer player;
    while (true) {
        PcmJob job;
        long long first_play_delay_ms = -1;
        uint64_t first_play_generation = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!stop_ && formal_playback_started_ && pcm_queue_.empty() &&
                IsPlaybackActiveUnlocked()) {
                MarkUnderrunUnlocked();
            }
            cv_.wait(lock, [this]() {
                if (stop_) {
                    return true;
                }
                if (pcm_queue_.empty()) {
                    return false;
                }
                const PcmJobKind front_kind = pcm_queue_.front().kind;
                if (front_kind == PcmJobKind::FastAck || front_kind == PcmJobKind::Static) {
                    return true;
                }
                if (formal_playback_started_) {
                    return true;
                }
                size_t formal_count = 0;
                for (const auto& pending : pcm_queue_) {
                    if (pending.kind == PcmJobKind::Formal) {
                        ++formal_count;
                    }
                }
                return formal_count >= min_start_pcm_chunks_ ||
                       (!synth_busy_ && text_queue_.empty());
            });
            if (stop_) {
                break;
            }
            job = std::move(pcm_queue_.front());
            pcm_queue_.pop_front();
            if (job.kind == PcmJobKind::Formal) {
                formal_playback_started_ = true;
            }
            RefreshProtectionLatchUnlocked();
        }
        if (job.pcm.empty()) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (job.generation != generation_) {
                continue;
            }
            if (stats_generation_ == job.generation && stats_started_ && !first_pcm_played_) {
                first_pcm_played_ = true;
                first_play_generation = job.generation;
                first_play_delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - first_text_enqueue_tp_)
                                          .count();
            }
        }
        if (first_play_delay_ms >= 0) {
            LogInfo("TtsWorker: generation=%llu first pcm played in %lld ms",
                    static_cast<unsigned long long>(first_play_generation),
                    first_play_delay_ms);
        }
        if (!player.PlayPcm(job.pcm, SAMPLE_RATE)) {
            LogWarn("TtsWorker: play pcm failed");
            continue;
        }
    }
}
