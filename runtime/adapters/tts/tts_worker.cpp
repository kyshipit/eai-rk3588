/*
 * adapters/tts/tts_worker.cpp
 */
#include "tts_worker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "audio_player.h"
#include "melotts_process.h"
#include "platform/logging.h"
#include "split.hpp"
#include "tts_text_sanitizer.h"

namespace {

constexpr float kSilenceThreshold = 5e-5f;
constexpr size_t kSilenceKeepPad = 441;
constexpr size_t kMaxLeadingScan = 22050;

// 仅裁掉 PCM 开头绝对静音（单样本近零），不碰弱起音，避免误删整句。
void TrimAbsoluteLeadingSilence(std::vector<float>& pcm) {
    if (pcm.size() < kSilenceKeepPad * 2) {
        return;
    }
    size_t start = 0;
    for (; start < pcm.size() && start < kMaxLeadingScan; ++start) {
        if (std::fabs(pcm[start]) >= kSilenceThreshold) {
            break;
        }
    }
    if (start == 0 || start >= pcm.size()) {
        return;
    }
    start = (start > kSilenceKeepPad) ? (start - kSilenceKeepPad) : 0;
    pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(start));
}

// 裁掉 PCM 末尾连续近零样本，减轻 Melo 分句拼接时的句间空白。
void TrimTrailingSilence(std::vector<float>& pcm) {
    if (pcm.size() < kSilenceKeepPad * 2) {
        return;
    }
    size_t end = pcm.size();
    while (end > 0 && std::fabs(pcm[end - 1]) < kSilenceThreshold) {
        --end;
    }
    if (end == pcm.size()) {
        return;
    }
    end = std::min(pcm.size(), end + kSilenceKeepPad);
    pcm.resize(end);
}

// 将 Melo 分句 PCM 拼成一段；每句先裁绝对静音，job 内合并后一次播放，避免块间 underrun。
void AppendMeloSentencePcm(std::vector<float>& merged, std::vector<float> chunk) {
    if (chunk.empty()) {
        return;
    }
    TrimAbsoluteLeadingSilence(chunk);
    merged.insert(merged.end(), std::make_move_iterator(chunk.begin()),
                  std::make_move_iterator(chunk.end()));
}

// 短答与问候相同：整段一次 Melo，不走合并/二次句首裁剪。
bool UseGreetingStyleSynth(const MeloTtsConfig& cfg, const std::string& text) {
    if (cfg.single_shot_max_chars <= 0) {
        return false;
    }
    return utf8_strlen(text) <= static_cast<size_t>(cfg.single_shot_max_chars);
}

}  // namespace

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
    const std::string cleaned = TtsTextSanitizer::Sanitize(text, max_speak_chars_);
    if (cleaned.empty()) {
        return;
    }
    TextJobKind kind = TextJobKind::FormalAnswer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (UseGreetingStyleSynth(cfg_, cleaned)) {
            kind = TextJobKind::Static;
        }
    }
    EnqueueCleaned(cleaned, kind);
}

// 兼容旧接口。
void TtsWorker::EnqueueSentence(const std::string& text) {
    EnqueueFormalAnswer(text);
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

// 锁内判断：合成/文本/PCM 队列非空即活跃播报。
bool TtsWorker::IsPlaybackActiveUnlocked() const {
    return synth_busy_ || !text_queue_.empty() || !pcm_queue_.empty();
}

// 锁内判断：正式回答是否仍有在途合成或待合成文本（不含已在 gst 的 PCM）。
bool TtsWorker::IsFormalPipelinePendingUnlocked() const {
    return synth_busy_ || !text_queue_.empty();
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

// 合并连续 FormalAnswer 至约 split_min_chars*8 字节，一次 decoder 批次产出多句 PCM。
std::string TtsWorker::CoalesceFormalAnswerLocked(TextJob first) {
    const int split_min = std::max(4, cfg_.split_min_chars);
    const size_t max_bytes = static_cast<size_t>(split_min * 8);
    std::string merged = std::move(first.text);
    while (!text_queue_.empty()) {
        const TextJob& next = text_queue_.front();
        if (next.generation != first.generation || next.kind != TextJobKind::FormalAnswer) {
            break;
        }
        if (merged.size() + next.text.size() > max_bytes) {
            break;
        }
        merged += next.text;
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

// 合成线程：FIFO 取文本并产出 PCM；FormalAnswer 先合并再推理以减少 decoder 往返。
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
            if (job.kind == TextJobKind::FormalAnswer) {
                const size_t min_batch =
                    static_cast<size_t>(std::max(24, cfg_.split_min_chars * 3));
                job.text = CoalesceFormalAnswerLocked(std::move(job));
                for (int attempt = 0; attempt < 2 && job.text.size() < min_batch && !stop_;
                     ++attempt) {
                    const bool got_more = cv_.wait_for(
                        lock, std::chrono::milliseconds(120), [this, &job]() {
                            if (stop_) {
                                return true;
                            }
                            if (text_queue_.empty()) {
                                return false;
                            }
                            const TextJob& next = text_queue_.front();
                            return next.generation == job.generation &&
                                   next.kind == TextJobKind::FormalAnswer;
                        });
                    if (!got_more) {
                        break;
                    }
                    TextJob batch;
                    batch.kind = TextJobKind::FormalAnswer;
                    batch.generation = job.generation;
                    batch.text = std::move(job.text);
                    job.text = CoalesceFormalAnswerLocked(std::move(batch));
                }
            } else {
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
        const uint64_t job_generation = job.generation;
        const TextJobKind job_kind = job.kind;
        bool pushed = false;
        if (job_kind == TextJobKind::FormalAnswer) {
            // 长答：Melo 多分句时 job 内合并 PCM，一次 PlayPcm，避免块间断粮。
            std::vector<float> merged_pcm;
            const bool emitted = session_.SynthesizeTextStreaming(
                job.text, [&merged_pcm](std::vector<float>&& pcm_chunk) {
                    AppendMeloSentencePcm(merged_pcm, std::move(pcm_chunk));
                    return true;
                });
            if (emitted && !merged_pcm.empty()) {
                TrimTrailingSilence(merged_pcm);
                pushed = PushPcmChunk(job_generation, std::move(merged_pcm), PcmJobKind::Formal);
            }
        } else {
            pushed = session_.SynthesizeTextStreaming(
                job.text, [this, job_generation](std::vector<float>&& pcm_chunk) {
                    if (!pcm_chunk.empty()) {
                        TrimAbsoluteLeadingSilence(pcm_chunk);
                    }
                    return PushPcmChunk(job_generation, std::move(pcm_chunk), PcmJobKind::Static);
                });
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            synth_busy_ = false;
            RefreshProtectionLatchUnlocked();
        }
        if (!pushed && job.kind == TextJobKind::FormalAnswer) {
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
            if (formal_playback_started_ && pcm_queue_.empty() &&
                !IsFormalPipelinePendingUnlocked()) {
                formal_playback_started_ = false;
            }
            if (!stop_ && formal_playback_started_ && pcm_queue_.empty() &&
                IsFormalPipelinePendingUnlocked()) {
                MarkUnderrunUnlocked();
            }
            cv_.wait(lock, [this]() {
                if (stop_) {
                    return true;
                }
                // Static/Formal 有 PCM 即播；Formal 不再等整 job 合成完，与问候 PlayText 一致。
                return !pcm_queue_.empty();
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
