/*
 * platform/model_coordinator.h
 *
 * 按需多槽：yolo / scrfd + llm 逻辑槽（无 OCR）。
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

enum class CoordinatorScene { Idle, Person };

class ModelCoordinator {
public:
    ModelCoordinator();

    // 初始化默认槽位原型并立即启用。
    bool Init(const std::string& default_model_name,
              std::shared_ptr<IModelAdapter> default_adapter,
              const std::string& model_path,
              const std::vector<int>& npu_cores,
              int num_infer_threads);

    // 注册已构造好的模型原型。
    void RegisterModel(const std::string& name, std::shared_ptr<IModelAdapter> adapter,
                       const std::string& model_path);
    // 注册懒加载工厂。
    void RegisterFactory(const std::string& name,
                         std::function<std::shared_ptr<IModelAdapter>()> factory,
                         const std::string& model_path);

    // 槽位策略配置。
    void SetSlotOptions(bool yolo_always_on);
    void SetSceneDwellFrames(int frames);

    // 预热某个槽位并放入 warm 池。
    bool WarmupSlot(const std::string& name);

    // 运行态查询。
    std::vector<std::pair<std::string, std::shared_ptr<IModelAdapter>>> GetEnabledSlotAdapters() const;
    std::string GetEnabledSlotsBadge() const;
    bool ShouldSuppressYoloPersonDraw() const;
    // 获取会话门控对象。
    LlmGreeting& GetLlmGreeting();

    // 设置 person 出现/消失阈值。
    void SetSwitchDebounceThresholds(int present_threshold, int absent_threshold);

    // 每帧更新：信号合并、去抖、槽位切换与 LLM 门控驱动。
    void UpdateAfterFrame(const AdapterSignals& signals, const cv::Mat& frame);

private:
    struct ModelEntry {
        std::shared_ptr<IModelAdapter> prototype;
        std::string model_path;
    };

    struct SlotRuntime {
        std::shared_ptr<IModelAdapter> adapter;
    };

    struct SlotPlan {
        CoordinatorScene scene = CoordinatorScene::Idle;
        bool want_yolo = false;
        bool want_scrfd = false;
    };

    bool IsModelAvailableUnlocked(const std::string& name) const;
    bool EnsureModelInPoolUnlocked(const std::string& name);
    bool EnableSlot(const std::string& name);
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
    CoordinatorScene current_scene_ = CoordinatorScene::Idle;
    CoordinatorScene applied_scene_ = CoordinatorScene::Idle;
    CoordinatorScene pending_scene_ = CoordinatorScene::Idle;
    int scene_dwell_count_ = 0;
    int scene_dwell_frames_ = 5;
    std::string last_logged_scene_;

    SharedState shared_state_;
    LlmGreeting llm_greeting_;

    int person_present_count_ = 0;
    int person_absent_count_ = 0;
    int present_threshold_ = 15;
    int absent_threshold_ = 30;

    mutable std::mutex mutex_;
};
