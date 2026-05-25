/*
 * engine/pipeline.h
 *
 * 【engine 层】调度内核：队列、线程池、任务流转。
 */
#pragma once
#include <atomic>
#include <chrono>
#include <future>
#include <functional>
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
#include "capture/camera_source.h"
#include "capture/frame_transform.h"
#include "display/display_sink.h"
#include "display/result_overlay.h"

struct SlotInferenceResult {
    // 槽位名（yolo/scrfd）。
    std::string slot;
    // Postprocess 输出的标准行文本。
    std::string result_json;
    // 槽位导出的行为信号。
    AdapterSignals signals;
};

struct InferenceTask {
    // 帧编号，-1 作为退出哨兵。
    int frame_id = 0;
    // 当前帧图像（供绘制与显示）。
    cv::Mat original_frame;
    // 各槽位推理结果。
    std::vector<SlotInferenceResult> slot_results;
    // 合并后的帧级信号。
    AdapterSignals merged_signals;
};

class Pipeline {
public:
    // 构造时完成相机打开、默认模型初始化与线程/队列资源准备。
    Pipeline(ModelCoordinator& coordinator,
             CameraSource& camera,
             FrameTransform& frame_transform,
             ResultOverlay& overlay,
             IDisplaySink& display,
             std::shared_ptr<IModelAdapter> base_adapter,
             const std::string& model_path,
             int num_infer_threads,
             const std::vector<int>& npu_cores,
             bool single_thread = false);
    ~Pipeline();

    // 主运行入口（单线程或多线程流水线模式）。
    void Run();
    // 请求优雅停止。
    void Stop();

    // 注册按需模型工厂（例如 scrfd）。
    void RegisterFactory(const std::string& name,
                         std::function<std::shared_ptr<IModelAdapter>()> factory,
                         const std::string& model_path);
    // 设置 scene 切换阈值。
    void SetSwitchDebounceThresholds(int present_threshold, int absent_threshold);
    // 注入外部停止标志（如信号处理器）。
    void SetExternalStopFlag(std::atomic<bool>* flag);

private:
    // 内部生命周期与线程管理。
    bool ShouldStop() const;
    void JoinWorkerThreads();
    // 各阶段循环。
    void PreprocessLoop();
    void InferenceLoop(int thread_id);
    void RunSingleThreaded();
    void RunPostprocessOnMainThread();
    // 显示阶段任务处理。
    bool ProcessDisplayTask(InferenceTask& task);
    // Stop 辅助：清队列并投递 quit 任务。
    void DrainPostQueue();
    void DrainInferQueues();
    void PushQuitTasksBestEffort();
    // 终端输入轮询与行缓冲解析。
    void PollTerminalPromptInput();
    // post 队列空时仍泵送 OpenCV 按键与 stdin，避免窗口卡死、ESC 无效。
    void PumpDisplayWhenIdle();

    ModelCoordinator& coordinator_;
    CameraSource& camera_;
    FrameTransform& frame_transform_;
    ResultOverlay& overlay_;
    IDisplaySink& display_;

    std::string model_path_;
    int num_infer_threads_;
    std::vector<int> npu_cores_;

    std::vector<std::unique_ptr<BoundedQueue<InferenceTask>>> infer_queues_;
    BoundedQueue<InferenceTask> post_queue_;

    std::unique_ptr<ThreadPool> infer_pool_;
    std::vector<std::future<void>> infer_futures_;
    std::thread pre_thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool>* external_stop_ = nullptr;
    std::atomic<uint64_t> processed_frames_{0};
    bool single_thread_ = false;
    std::chrono::steady_clock::time_point start_time_;
    std::string last_display_badge_;
    std::string terminal_input_buffer_;

    static AdapterSignals MergeSlotSignals(const std::vector<SlotInferenceResult>& slot_results);
    static bool RunEnabledSlots(ModelCoordinator& coordinator, const cv::Mat& frame,
                                std::vector<SlotInferenceResult>& slot_results,
                                AdapterSignals& merged_signals);
};
