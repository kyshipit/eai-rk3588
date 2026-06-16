/*
 * engine/pipeline.h
 *
 * 纯调度框架：队列、线程池、Preprocess→Inference→Postprocess 任务流转。
 * 不含相机、显示、终端或具体模型名；由 app 注入帧源与后处理回调。
 */
#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <opencv2/opencv.hpp>

#include "bounded_queue.h"
#include "thread_pool.h"
#include "adapter_interface.h"
#include "platform/adapter_signals.h"
#include "platform/model_coordinator.h"

struct SlotInferenceResult {
    // 槽位名（由 ModelCoordinator 登记，engine 不硬编码）。
    std::string slot;
    // Postprocess 输出的标准行文本。
    std::string result_json;
    // 槽位导出的行为信号。
    AdapterSignals signals;
};

struct InferenceTask {
    // 帧编号，-1 作为退出哨兵。
    int frame_id = 0;
    // 当前帧图像（供 app 层绘制与显示）。
    cv::Mat original_frame;
    // 各槽位推理结果。
    std::vector<SlotInferenceResult> slot_results;
    // 合并后的帧级信号。
    AdapterSignals merged_signals;
};

class Pipeline {
public:
    using FrameReadFn = std::function<bool(cv::Mat& frame)>;
    using FramePreprocessFn = std::function<void(cv::Mat& frame)>;
    using FrameValidateFn = std::function<bool(const cv::Mat& frame, int frame_id)>;
    // 返回 false 时主循环退出（如 quit 哨兵或 app 请求停止）。
    using PostTaskFn = std::function<bool(InferenceTask& task)>;

    // 仅创建队列与线程池；模型 Init 与相机打开由 app 负责。
    explicit Pipeline(ModelCoordinator& coordinator,
                      int num_infer_threads,
                      bool single_thread = false);
    ~Pipeline();

    void Run();
    void Stop();

    void RegisterFactory(const std::string& name,
                         std::function<std::shared_ptr<IModelAdapter>()> factory,
                         const std::string& model_path);
    void SetSwitchDebounceThresholds(int present_threshold, int absent_threshold);
    void SetExternalStopFlag(std::atomic<bool>* flag);

    // 注入采帧与变换（通常绑定 CameraSource + FrameTransform）。
    void SetFrameIO(FrameReadFn read, FramePreprocessFn preprocess, FrameValidateFn validate);
    // 每帧推理完成后的 app 回调（显示、门控、stdin 等）。
    void SetPostTaskHandler(PostTaskFn handler);
    // 队列空时泵送（ConsoleUi、按键等）。
    void SetIdleHandler(std::function<void()> handler);
    void SetOnStop(std::function<void()> handler);

private:
    bool ShouldStop() const;
    void JoinWorkerThreads();
    void PreprocessLoop();
    void InferenceLoop(int thread_id);
    void RunSingleThreaded();
    void RunPostprocessOnMainThread();
    void DrainPostQueue();
    void DrainInferQueues();
    void PushQuitTasksBestEffort();
    void PumpIdle();

    ModelCoordinator& coordinator_;
    int num_infer_threads_;

    std::vector<std::unique_ptr<BoundedQueue<InferenceTask>>> infer_queues_;
    BoundedQueue<InferenceTask> post_queue_;

    std::unique_ptr<ThreadPool> infer_pool_;
    std::vector<std::future<void>> infer_futures_;
    std::thread pre_thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool>* external_stop_ = nullptr;
    bool single_thread_ = false;

    FrameReadFn frame_read_;
    FramePreprocessFn frame_preprocess_;
    FrameValidateFn frame_validate_;
    PostTaskFn post_task_;
    std::function<void()> idle_handler_;
    std::function<void()> on_stop_;

    static AdapterSignals MergeSlotSignals(const std::vector<SlotInferenceResult>& slot_results);
    static bool RunEnabledSlots(ModelCoordinator& coordinator, const cv::Mat& frame, int frame_id,
                                std::vector<SlotInferenceResult>& slot_results,
                                AdapterSignals& merged_signals);
};
