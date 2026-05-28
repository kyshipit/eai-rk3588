/*
 * adapters/tts/tts_worker.h — 异步 MeloTTS 播报：仅播放 AI> 助手话术队列（最新优先）。
 */
#pragma once

#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>

#include "melotts_session.h"

class TtsWorker {
public:
    TtsWorker();
    ~TtsWorker();

    void Configure(const MeloTtsConfig& cfg, int max_speak_chars);
    void RequestInitializeAsync();
    void PollInitState();

    // 替换待播内容为最新一段（不 FIFO 堆叠）；若未 ready 则丢弃。
    void PlayText(const std::string& text);
    void Cancel();
    void Shutdown();

    bool IsReady() const;
    bool IsInitializing() const;

private:
    void WorkerLoop();
    void JoinWorkerThread();
    bool IsReadyUnlocked() const;

    enum class InitState { Uninitialized, Initializing, Ready, Failed };

    MeloTtsConfig cfg_;
    int max_speak_chars_ = 0;
    MeloTtsSession session_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    InitState init_state_ = InitState::Uninitialized;
    std::future<bool> init_future_;

    std::thread worker_thread_;
    bool stop_ = false;
    bool has_job_ = false;
    std::string pending_text_;
    bool configured_ = false;
};
