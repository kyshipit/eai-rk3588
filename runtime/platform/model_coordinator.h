/*
 * platform/model_coordinator.h
 *
 * 方案 A：按需激活多槽（yolo / scrfd / ppocr + llm 逻辑槽）。
 */
#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <functional>
#include <opencv2/opencv.hpp>

#include "engine/adapter_interface.h"
#include "shared_state.h"
#include "adapter_signals.h"
#include "llm_greeting.h"

class PpocrTextRegionProbe;

// 去抖后的视觉场景，决定本帧应启用哪些槽（见 docs/多模型并行问题.md）。
enum class CoordinatorScene { Idle, Person, TextOnly, Document };

class ModelCoordinator {
public:
    ModelCoordinator();

    // Pipeline 构造时调用：登记默认 adapter，并在锁外 Enable 默认槽（禁止在持 mutex_ 时调 EnableSlot）。
    bool Init(const std::string& default_model_name,
              std::shared_ptr<IModelAdapter> default_adapter,
              const std::string& model_path,
              const std::vector<int>& npu_cores,
              int num_infer_threads);

    // 是否至少有一个 enabled 槽，供 Pipeline 判断能否推理。
    bool HasActiveAdapters() const;

    void RegisterModel(const std::string& name, std::shared_ptr<IModelAdapter> adapter);
    void RegisterModel(const std::string& name, std::shared_ptr<IModelAdapter> adapter,
                       const std::string& model_path);
    void RegisterFactory(const std::string& name,
                         std::function<std::shared_ptr<IModelAdapter>()> factory,
                         const std::string& model_path);

    void SetTextRegionProbe(std::shared_ptr<PpocrTextRegionProbe> probe);
    void SetSlotOptions(bool yolo_always_on, int text_probe_interval_frames,
                        bool document_disable_yolo = true);
    void SetSceneDwellFrames(int frames);

    // 启动时预 Init 并放入 warm 池，避免运行中首次切槽长时间阻塞；须在 RegisterFactory 之后调用。
    bool WarmupSlot(const std::string& name);

    // 返回当前 enabled 槽及 adapter 副本，供 RunEnabledSlots 顺序推理（短持锁）。
    std::vector<std::pair<std::string, std::shared_ptr<IModelAdapter>>> GetEnabledSlotAdapters() const;
    std::string GetEnabledSlotsBadge() const;
    std::string GetCurrentModelName() const;
    std::string GetCurrentScene() const;
    // 三槽同开时为 true，绘制层据此抑制 YOLO person 框。
    bool ShouldSuppressYoloPersonDraw() const;

    SharedState& GetSharedState();
    LlmGreeting& GetLlmGreeting();

    void SetSwitchDebounceThresholds(int present_threshold, int absent_threshold,
                                     int text_absent_threshold = 30);
    void SetToPpocrMode(const std::string& mode);
    void SetPpocrBackPolicy(bool auto_disable_ppocr_on_no_text, int min_ppocr_frames,
                            bool back_on_no_text);

    // 推理后调用：按间隔跑 det 探针更新 text_region_present；NPU 在锁外执行。
    bool ProbeTextRegion(const cv::Mat& frame, AdapterSignals& in_out_signals);
    // 显示帧后调用：去抖、场景驻留、ApplySlotPlan 启停槽（槽变更在锁外）。
    void UpdateAfterFrame(const AdapterSignals& signals, const cv::Mat& frame);

private:
    struct ModelEntry {
        std::shared_ptr<IModelAdapter> prototype;
        std::string model_path;
    };

    struct SlotRuntime {
        std::shared_ptr<IModelAdapter> adapter;
    };

    // 单帧目标槽组合；由 BuildSlotPlanUnlocked 填充，ApplySlotPlan 执行。
    struct SlotPlan {
        CoordinatorScene scene = CoordinatorScene::Idle;
        bool want_yolo = false;
        bool want_scrfd = false;
        bool want_ppocr = false;
    };

    bool IsModelAvailableUnlocked(const std::string& name) const;
    bool EnsureModelInPoolUnlocked(const std::string& name);
    // 启用槽：禁止在已持 mutex_ 时调用；RKNN Init 在锁外。可从 warm 池复用。
    bool EnableSlot(const std::string& name);
    // 禁用槽：adapter 移入 warm_runtimes_，不销毁 RKNN。
    void DisableSlot(const std::string& name);
    SlotPlan BuildSlotPlanUnlocked();
    void ApplySlotPlan(const SlotPlan& plan);
    void MergeSignalsUnlocked(const AdapterSignals& signals);
    CoordinatorScene ComputeSceneUnlocked() const;
    static const char* SceneName(CoordinatorScene scene);
    int CoreMaskForSlot(const std::string& slot) const;
    static std::vector<std::string> OrderedSlotNames();

    std::unordered_map<std::string, ModelEntry> model_pool_;
    std::unordered_map<std::string, std::function<std::shared_ptr<IModelAdapter>()>> factory_map_;
    std::unordered_map<std::string, std::string> factory_path_map_;
    std::unordered_map<std::string, SlotRuntime> slot_runtimes_;
    std::unordered_map<std::string, SlotRuntime> warm_runtimes_;
    std::unordered_set<std::string> enabled_slots_;

    std::vector<int> npu_cores_;
    int num_infer_threads_ = 1;
    bool yolo_always_on_ = true;
    bool document_disable_yolo_ = true;
    int text_probe_interval_ = 5;
    CoordinatorScene current_scene_ = CoordinatorScene::Idle;
    CoordinatorScene applied_scene_ = CoordinatorScene::Idle;
    CoordinatorScene pending_scene_ = CoordinatorScene::Idle;
    int scene_dwell_count_ = 0;
    int scene_dwell_frames_ = 5;
    std::string last_logged_scene_;
    int text_probe_frame_counter_ = 0;

    SharedState shared_state_;
    AdapterSignals last_signals_;
    LlmGreeting llm_greeting_;
    std::shared_ptr<PpocrTextRegionProbe> text_probe_;

    int person_present_count_ = 0;
    int person_absent_count_ = 0;
    int text_region_present_count_ = 0;
    int text_region_absent_count_ = 0;
    int face_absent_count_ = 0;
    int present_threshold_ = 15;
    int absent_threshold_ = 30;
    int text_absent_count_ = 0;
    int text_absent_threshold_ = 30;
    std::string to_ppocr_mode_ = "text";
    int ppocr_lab_frame_count_ = 0;
    bool auto_disable_ppocr_ = true;
    bool back_on_no_text_ = true;
    int min_ppocr_frames_ = 90;
    int ppocr_active_frames_ = 0;

    mutable std::mutex mutex_;
};
