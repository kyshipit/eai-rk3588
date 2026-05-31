/*
 * adapters/tts/tts_worker.h — 异步 MeloTTS 播报：文本合成与播放并行流水线。
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "melotts_session.h"

class TtsWorker {
public:
    // 默认构造；模型在 Configure + RequestInitializeAsync 后才加载。
    TtsWorker();
    // 停止后台线程并释放 RKNN 会话。
    ~TtsWorker();

    // 保存合成参数与播报长度上限，不立即加载模型。
    void Configure(const MeloTtsConfig& cfg, int max_speak_chars);
    // 异步加载 Lexicon 与 encoder/decoder RKNN（幂等）。
    void RequestInitializeAsync();
    // 主线程轮询 init 结果，成功后启动合成/播放线程。
    void PollInitState();

    // 将一段文本清洗后追加到待合成队列（FIFO）。
    void PlayText(const std::string& text);
    // 将流式正式回答片段清洗后追加到待合成队列。
    void EnqueueFormalAnswer(const std::string& text);
    // 将流式分块文本清洗后追加到待合成队列（兼容旧接口，等同 EnqueueFormalAnswer）。
    void EnqueueSentence(const std::string& text);
    // 配置短反馈文案；TTS ready 后预合成缓存 PCM。
    void ConfigureFastAck(bool enabled, const std::string& text);
    // TTS init 完成后预合成短反馈 PCM（幂等）。
    void BuildFastAckCacheIfNeeded();
    // 播放预缓存短反馈 PCM，不等待 min_start_pcm_chunks。
    void PlayFastAck();
    // 停止播放并清空待播队列。
    void Cancel();
    // 退出 worker 线程并释放 MeloTtsSession。
    void Shutdown();

    // 是否已完成异步初始化且可合成。
    bool IsReady() const;
    // 是否正在后台加载模型。
    bool IsInitializing() const;
    // 返回 max_speak_chars 配置（供流式分块清洗对齐）。
    int MaxSpeakChars() const;
    // 配置播放保护水位阈值（按 PCM 片段数）；high 应大于 low。
    void SetPlaybackProtectionThresholds(size_t low_chunks, size_t high_chunks);
    // 配置首段开播前的最小缓冲片段数，减少正式回答中途断粮。
    void SetMinStartPcmChunks(size_t chunks);
    // 当前是否处于播报保护窗口（建议上层临时降视觉负载）。
    bool NeedPlaybackProtection() const;
    // 当前是否存在待合成/待播放任务（含合成中）。
    bool IsPlaybackActive() const;

private:
    enum class TextJobKind {
        Static,
        FormalAnswer,
    };
    enum class PcmJobKind {
        FastAck,
        Static,
        Formal,
    };

    struct TextJob {
        TextJobKind kind = TextJobKind::FormalAnswer;
        uint64_t generation = 0;
        std::string text;
    };
    struct PcmJob {
        PcmJobKind kind = PcmJobKind::Formal;
        uint64_t generation = 0;
        std::vector<float> pcm;
    };

    // 清洗后入队；未 ready 时触发 init，保留排队文本等待模型就绪。
    void EnqueueCleaned(std::string cleaned, TextJobKind kind);
    // 合并同一代际下少量连续文本块，避免过短分句造成过多 RKNN 往返。
    std::string CoalescePendingTextLocked(TextJob first);
    // 将单个 PCM 片段入队；若代际已切换则返回 false 终止后续合成。
    bool PushPcmChunk(uint64_t generation, std::vector<float> pcm,
                      PcmJobKind kind = PcmJobKind::Formal);
    // 重置指定代际的首响/断粮统计状态。
    void ResetGenerationStatsUnlocked(uint64_t generation);
    // 在代际结束时输出统计摘要（首响、断粮次数）。
    void FinalizeGenerationStatsUnlocked();
    // 记录一次连续断粮窗口（仅边沿计数，避免刷屏）。
    void MarkUnderrunUnlocked();
    // 刷新播放保护锁存状态（低水位进入，高水位退出）。
    void RefreshProtectionLatchUnlocked();
    // 锁内判断当前是否处于活跃播报阶段。
    bool IsPlaybackActiveUnlocked() const;
    // 合成线程：FIFO 取文本并产出 PCM，同轮多块合并后再推理。
    void SynthesizeLoop();
    // 播放线程：FIFO 取 PCM 连续播放。
    void PlaybackLoop();
    // join 后台线程（Shutdown 用）。
    void JoinWorkerThreads();
    // 调用方已持 mutex_ 时的 ready 判定。
    bool IsReadyUnlocked() const;

    enum class InitState { Uninitialized, Initializing, Ready, Failed };

    MeloTtsConfig cfg_;
    int max_speak_chars_ = 0;
    MeloTtsSession session_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    InitState init_state_ = InitState::Uninitialized;
    std::future<bool> init_future_;

    std::thread synth_thread_;
    std::thread play_thread_;
    bool stop_ = false;
    uint64_t generation_ = 0;
    std::deque<TextJob> text_queue_;
    std::deque<PcmJob> pcm_queue_;
    bool synth_busy_ = false;
    size_t protect_low_chunks_ = 1;
    size_t protect_high_chunks_ = 3;
    size_t min_start_pcm_chunks_ = 2;
    bool protect_latched_ = false;
    uint64_t stats_generation_ = 0;
    bool stats_started_ = false;
    bool stats_finalized_ = false;
    bool first_pcm_enqueued_ = false;
    bool first_pcm_played_ = false;
    bool underrun_latched_ = false;
    uint32_t underrun_count_ = 0;
    std::chrono::steady_clock::time_point first_text_enqueue_tp_ = std::chrono::steady_clock::now();
    bool configured_ = false;
    bool fast_ack_enabled_ = false;
    std::string fast_ack_text_;
    std::vector<float> fast_ack_pcm_;
    bool fast_ack_cache_built_ = false;
    bool formal_playback_started_ = false;
};
