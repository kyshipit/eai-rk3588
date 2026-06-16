/*
 * engine/pipeline.cpp — 纯调度内核：不含相机/显示/业务模型名
 */
#include "pipeline.h"
#include "platform/logging.h"
#include <chrono>

// 合并本帧各槽 GetAdapterSignals（OR），供 app 层 UpdateAfterFrame 去抖。
AdapterSignals Pipeline::MergeSlotSignals(const std::vector<SlotInferenceResult>& slot_results) {
    AdapterSignals merged;
    for (const auto& r : slot_results) {
        merged.person_present = merged.person_present || r.signals.person_present;
        merged.face_detected = merged.face_detected || r.signals.face_detected;
        if (r.signals.avg_brightness > 0.0f) {
            merged.avg_brightness = r.signals.avg_brightness;
        }
        if (!r.signals.scene_label.empty()) {
            merged.scene_label = r.signals.scene_label;
        }
    }
    return merged;
}

// 对当前 enabled 槽顺序 Preprocess→Inference→Postprocess；降帧策略由 coordinator 决定。
bool Pipeline::RunEnabledSlots(ModelCoordinator& coordinator, const cv::Mat& frame, int frame_id,
                               std::vector<SlotInferenceResult>& slot_results,
                               AdapterSignals& merged_signals) {
    slot_results.clear();
    auto slots = coordinator.GetEnabledSlotAdapters();
    if (slots.empty()) {
        return false;
    }
    for (const auto& entry : slots) {
        if (!entry.second) {
            continue;
        }
        if (!coordinator.ShouldRunSlotInference(entry.first, frame_id)) {
            continue;
        }
        int input_size = 0;
        if (entry.second->Preprocess(frame, input_size) == nullptr || input_size <= 0) {
            continue;
        }
        std::shared_ptr<void> model_output;
        if (entry.second->Inference(model_output) != 0) {
            model_output.reset();
        }
        SlotInferenceResult one;
        one.slot = entry.first;
        one.result_json = entry.second->Postprocess(model_output);
        one.signals = entry.second->GetAdapterSignals();
        slot_results.push_back(std::move(one));
    }
    merged_signals = MergeSlotSignals(slot_results);
    return !slot_results.empty();
}

// 仅创建 infer/post 队列与线程池；不在此打开相机或 Init 默认槽。
Pipeline::Pipeline(ModelCoordinator& coordinator, int num_infer_threads, bool single_thread)
    : coordinator_(coordinator),
      num_infer_threads_(num_infer_threads > 0 ? num_infer_threads : 1),
      post_queue_(4),
      single_thread_(single_thread) {
    infer_pool_.reset(new ThreadPool(num_infer_threads_));
    infer_queues_.reserve(num_infer_threads_);
    for (int i = 0; i < num_infer_threads_; ++i) {
        infer_queues_.push_back(
            std::unique_ptr<BoundedQueue<InferenceTask>>(new BoundedQueue<InferenceTask>(2)));
    }
}

Pipeline::~Pipeline() {
    if (!stop_.load()) {
        Stop();
    }
    JoinWorkerThreads();
}

void Pipeline::RegisterFactory(const std::string& name,
                               std::function<std::shared_ptr<IModelAdapter>()> factory,
                               const std::string& model_path) {
    coordinator_.RegisterFactory(name, std::move(factory), model_path);
}

void Pipeline::SetSwitchDebounceThresholds(int present_threshold, int absent_threshold) {
    coordinator_.SetSwitchDebounceThresholds(present_threshold, absent_threshold);
}

void Pipeline::SetExternalStopFlag(std::atomic<bool>* flag) {
    external_stop_ = flag;
}

// 绑定采帧、旋转/校验与每帧后处理（显示、门控等由 app 实现）。
void Pipeline::SetFrameIO(FrameReadFn read, FramePreprocessFn preprocess, FrameValidateFn validate) {
    frame_read_ = std::move(read);
    frame_preprocess_ = std::move(preprocess);
    frame_validate_ = std::move(validate);
}

void Pipeline::SetPostTaskHandler(PostTaskFn handler) {
    post_task_ = std::move(handler);
}

void Pipeline::SetIdleHandler(std::function<void()> handler) {
    idle_handler_ = std::move(handler);
}

void Pipeline::SetOnStop(std::function<void()> handler) {
    on_stop_ = std::move(handler);
}

void Pipeline::DrainPostQueue() {
    InferenceTask discarded;
    while (post_queue_.TryPop(discarded, 0)) {
    }
}

void Pipeline::DrainInferQueues() {
    InferenceTask discarded;
    for (auto& q : infer_queues_) {
        while (q->TryPop(discarded, 0)) {
        }
    }
}

bool Pipeline::ShouldStop() const {
    return stop_.load() || (external_stop_ != nullptr && external_stop_->load());
}

void Pipeline::JoinWorkerThreads() {
    if (pre_thread_.joinable()) {
        pre_thread_.join();
    }
    for (auto& fut : infer_futures_) {
        if (fut.valid()) {
            fut.wait();
        }
    }
    infer_futures_.clear();
}

void Pipeline::PushQuitTasksBestEffort() {
    DrainPostQueue();
    DrainInferQueues();
    InferenceTask quit_task;
    quit_task.frame_id = -1;
    const int kTimeoutMs = 500;
    for (int i = 0; i < num_infer_threads_; ++i) {
        if (!infer_queues_[i]->TryPush(quit_task, kTimeoutMs)) {
            LogWarn("Pipeline::Stop: infer_queue %d quit_task not delivered (queue full)", i);
        }
    }
    if (!post_queue_.TryPush(quit_task, kTimeoutMs)) {
        LogWarn("Pipeline::Stop: post_queue quit_task not delivered (queue full)");
    }
}

void Pipeline::Stop() {
    if (stop_.exchange(true)) {
        return;
    }
    LogInfo("Pipeline::Stop: stop requested");
    if (on_stop_) {
        on_stop_();
    }
    PushQuitTasksBestEffort();
}

void Pipeline::Run() {
    LogInfo("Pipeline::Run: begin (infer_threads=%d)", num_infer_threads_);
    if (!frame_read_) {
        LogFatal("Pipeline::Run: frame reader not set");
        throw std::runtime_error("Pipeline: SetFrameIO required before Run");
    }
    if (!post_task_) {
        LogFatal("Pipeline::Run: post task handler not set");
        throw std::runtime_error("Pipeline: SetPostTaskHandler required before Run");
    }

    if (single_thread_) {
        RunSingleThreaded();
    } else {
        pre_thread_ = std::thread(&Pipeline::PreprocessLoop, this);
        infer_futures_.reserve(static_cast<size_t>(num_infer_threads_));
        for (int i = 0; i < num_infer_threads_; ++i) {
            infer_futures_.push_back(
                infer_pool_->Enqueue([this, i]() { InferenceLoop(i); }));
        }
        RunPostprocessOnMainThread();
        JoinWorkerThreads();
    }
    LogInfo("Pipeline::Run: exited normally");
}

void Pipeline::RunSingleThreaded() {
    int frame_id = 0;
    while (!ShouldStop()) {
        PumpIdle();
        cv::Mat frame;
        if (!frame_read_(frame)) {
            if (ShouldStop()) {
                break;
            }
            continue;
        }
        if (frame_preprocess_) {
            frame_preprocess_(frame);
        }
        if (frame_validate_ && !frame_validate_(frame, frame_id)) {
            continue;
        }
        if (!frame.isContinuous()) {
            frame = frame.clone();
        }

        InferenceTask task;
        task.frame_id = frame_id++;
        task.original_frame = frame.clone();
        if (!RunEnabledSlots(coordinator_, frame, task.frame_id, task.slot_results,
                             task.merged_signals)) {
            continue;
        }
        if (!post_task_(task)) {
            break;
        }
    }
}

void Pipeline::PumpIdle() {
    if (idle_handler_) {
        idle_handler_();
    }
}

void Pipeline::PreprocessLoop() {
    int frame_id = 0;
    const int kPushTimeoutMs = 100;
    while (!ShouldStop()) {
        cv::Mat frame;
        if (!frame_read_(frame)) {
            if (ShouldStop()) {
                break;
            }
            continue;
        }
        if (frame_preprocess_) {
            frame_preprocess_(frame);
        }
        if (frame_validate_ && !frame_validate_(frame, frame_id)) {
            continue;
        }
        if (!frame.isContinuous()) {
            frame = frame.clone();
        }

        const int queue_index = frame_id % num_infer_threads_;
        InferenceTask task;
        task.frame_id = frame_id++;
        task.original_frame = frame.clone();
        if (!infer_queues_[queue_index]->TryPush(std::move(task), kPushTimeoutMs)) {
            if (ShouldStop()) {
                break;
            }
        }
    }
}

void Pipeline::InferenceLoop(int thread_id) {
    const int kPopTimeoutMs = 200;
    const int kPushTimeoutMs = 100;
    while (!ShouldStop()) {
        InferenceTask task;
        if (!infer_queues_[thread_id]->TryPop(task, kPopTimeoutMs)) {
            continue;
        }
        if (task.frame_id == -1) {
            break;
        }
        if (frame_validate_ && !frame_validate_(task.original_frame, task.frame_id)) {
            continue;
        }
        if (!RunEnabledSlots(coordinator_, task.original_frame, task.frame_id,
                             task.slot_results, task.merged_signals)) {
            continue;
        }
        if (!post_queue_.TryPush(std::move(task), kPushTimeoutMs)) {
            if (ShouldStop()) {
                break;
            }
        }
    }
}

void Pipeline::RunPostprocessOnMainThread() {
    LogInfo("Pipeline::RunPostprocessOnMainThread: entered");
    const int kPopTimeoutMs = 200;
    while (!ShouldStop()) {
        PumpIdle();
        InferenceTask task;
        if (!post_queue_.TryPop(task, kPopTimeoutMs)) {
            continue;
        }
        if (!post_task_(task)) {
            break;
        }
    }
    LogInfo("Pipeline::RunPostprocessOnMainThread: exiting");
}
