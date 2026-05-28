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
    // 默认构造；模型在 Configure + RequestInitializeAsync 后才加载。
    TtsWorker();
    // 停止后台线程并释放 RKNN 会话。
    ~TtsWorker();

    // 保存合成参数与播报长度上限，不立即加载模型。
    void Configure(const MeloTtsConfig& cfg, int max_speak_chars);
    // 异步加载 Lexicon 与 encoder/decoder RKNN（幂等）。
    void RequestInitializeAsync();
    // 主线程轮询 init 结果，成功后启动 WorkerLoop。
    void PollInitState();

    // 替换待播内容为最新一段（不 FIFO 堆叠）；未 ready 时触发 init 并丢弃本次文本。
    void PlayText(const std::string& text);
    // 停止播放并清空待播队列。
    void Cancel();
    // 退出 worker 线程并释放 MeloTtsSession。
    void Shutdown();

    // 是否已完成异步初始化且可合成。
    bool IsReady() const;
    // 是否正在后台加载模型。
    bool IsInitializing() const;

private:
    // 后台线程：取最新 pending 文本 → 合成 → gst-play 播放。
    void WorkerLoop();
    // join worker 线程（Shutdown 用）。
    void JoinWorkerThread();
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

    std::thread worker_thread_;
    bool stop_ = false;
    bool has_job_ = false;
    std::string pending_text_;
    bool configured_ = false;
};
