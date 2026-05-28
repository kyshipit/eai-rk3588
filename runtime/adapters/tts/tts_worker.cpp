/*
 * adapters/tts/tts_worker.cpp
 */
#include "tts_worker.h"

#include <chrono>

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
    std::lock_guard<std::mutex> lock(mutex_);
    init_state_ = ok ? InitState::Ready : InitState::Failed;
    if (ok) {
        LogInfo("TtsWorker: init ok");
        if (!worker_thread_.joinable()) {
            stop_ = false;
            worker_thread_ = std::thread(&TtsWorker::WorkerLoop, this);
        }
    } else {
        LogWarn("TtsWorker: init failed");
    }
}

// 提交最新一段待播文本（覆盖队列中旧内容）。
void TtsWorker::PlayText(const std::string& text) {
    const std::string cleaned = TtsTextSanitizer::Sanitize(text, max_speak_chars_);
    if (cleaned.empty()) {
        return;
    }
    if (!IsReady()) {
        RequestInitializeAsync();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_text_ = cleaned;
        has_job_ = true;
    }
    cv_.notify_one();
}

// 停止播放并清空待播。
void TtsWorker::Cancel() {
    AudioPlayer player;
    player.Stop();
    std::lock_guard<std::mutex> lock(mutex_);
    has_job_ = false;
    pending_text_.clear();
}

// 退出 worker 线程并 shutdown session。
void TtsWorker::Shutdown() {
    Cancel();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    JoinWorkerThread();
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

// 锁内 ready 判定。
bool TtsWorker::IsReadyUnlocked() const {
    return init_state_ == InitState::Ready;
}

// 等待 worker 线程结束。
void TtsWorker::JoinWorkerThread() {
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

// 后台：取最新 pending 文本 → 合成 → 播放。
void TtsWorker::WorkerLoop() {
    AudioPlayer player;
    while (true) {
        std::string text;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stop_ || has_job_; });
            if (stop_) {
                break;
            }
            text = pending_text_;
            has_job_ = false;
            pending_text_.clear();
        }
        if (text.empty()) {
            continue;
        }
        std::vector<float> pcm = session_.SynthesizeText(text);
        if (pcm.empty()) {
            LogWarn("TtsWorker: synthesize empty");
            continue;
        }
        player.PlayPcm(pcm, SAMPLE_RATE);
    }
}
