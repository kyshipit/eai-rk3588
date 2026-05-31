/*
 * platform/model_coordinator.cpp — yolo + scrfd 多槽
 */
#include "model_coordinator.h"
#include "logging.h"
#include <sstream>

namespace {

// 将配置中的核心编号/位掩码转换为 rknn_set_core_mask 可接受值。
int ToRknnCoreMask(int core_cfg) {
    if (core_cfg >= 1 && core_cfg <= 4 && (core_cfg & (core_cfg - 1)) == 0) {
        return core_cfg;
    }
    if (core_cfg >= 0 && core_cfg <= 2) {
        return 1 << core_cfg;
    }
    return 1;
}

}  // namespace

ModelCoordinator::ModelCoordinator() = default;

// 定义槽位展示与调度顺序，保证日志和 UI badge 稳定。
std::vector<std::string> ModelCoordinator::OrderedSlotNames() {
    return {"yolo", "scrfd"};
}

bool ModelCoordinator::Init(const std::string& default_model_name,
                            std::shared_ptr<IModelAdapter> default_adapter,
                            const std::string& model_path,
                            const std::vector<int>& npu_cores,
                            int num_infer_threads) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        npu_cores_ = npu_cores;
        num_infer_threads_ = num_infer_threads > 0 ? num_infer_threads : 1;
        enabled_slots_.clear();
        slot_runtimes_.clear();
        warm_runtimes_.clear();

        if (default_adapter) {
            model_pool_[default_model_name] = {default_adapter, model_path};
        }
        LogInfo("ModelCoordinator: init default='%s' path='%s'",
                default_model_name.c_str(), model_path.c_str());
    }

    if (!EnableSlot(default_model_name)) {
        LogError("ModelCoordinator: failed to enable default slot '%s'", default_model_name.c_str());
        return false;
    }
    return true;
}

// 注册已构造好的模型原型（直接放入模型池）。
void ModelCoordinator::RegisterModel(const std::string& name, std::shared_ptr<IModelAdapter> adapter,
                                     const std::string& model_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (adapter) {
        model_pool_[name] = {adapter, model_path};
        factory_path_map_[name] = model_path;
    }
}

// 注册懒构造工厂（按需启槽时再实例化）。
void ModelCoordinator::RegisterFactory(const std::string& name,
                                       std::function<std::shared_ptr<IModelAdapter>()> factory,
                                       const std::string& model_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    factory_map_[name] = std::move(factory);
    factory_path_map_[name] = model_path;
    LogInfo("ModelCoordinator: registered factory '%s' path='%s'", name.c_str(), model_path.c_str());
}

// 设置槽位策略选项（当前主要控制 yolo 是否常驻）。
void ModelCoordinator::SetSlotOptions(bool yolo_always_on) {
    std::lock_guard<std::mutex> lock(mutex_);
    yolo_always_on_ = yolo_always_on;
}

// 设置场景切换驻留帧数（去抖强度）。
void ModelCoordinator::SetSceneDwellFrames(int frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    scene_dwell_frames_ = frames > 0 ? frames : 1;
}

// 设置 TTS 播报低水位保护时是否允许临时视觉降载。
void ModelCoordinator::SetTtsVisualThrottleEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    tts_visual_throttle_enabled_ = enabled;
}

// 预热槽位：触发一次 Enable 初始化后立即转入 warm 池。
bool ModelCoordinator::WarmupSlot(const std::string& name) {
    if (!EnableSlot(name)) {
        return false;
    }
    DisableSlot(name);
    LogInfo("ModelCoordinator: warmup slot '%s' (in warm pool)", name.c_str());
    return true;
}

// 判断模型是否可用（原型已存在或存在工厂可构造）。
bool ModelCoordinator::IsModelAvailableUnlocked(const std::string& name) const {
    return model_pool_.count(name) > 0 || factory_map_.count(name) > 0;
}

// 确保模型池中有可克隆原型；若无则尝试用工厂懒加载。
bool ModelCoordinator::EnsureModelInPoolUnlocked(const std::string& name) {
    auto pool_it = model_pool_.find(name);
    if (pool_it != model_pool_.end()) {
        return pool_it->second.prototype != nullptr;
    }
    auto factory_it = factory_map_.find(name);
    if (factory_it == factory_map_.end()) {
        return false;
    }
    auto adapter = factory_it->second();
    if (!adapter) {
        return false;
    }
    std::string path = factory_path_map_[name];
    model_pool_[name] = {adapter, path};
    LogInfo("ModelCoordinator: lazy-loaded prototype for '%s'", name.c_str());
    return true;
}

// 根据槽位名选择 NPU 核心掩码（yolo/scrfd 可分核）。
int ModelCoordinator::CoreMaskForSlot(const std::string& slot) const {
    int idx = (slot == "scrfd") ? 1 : 0;
    if (idx < static_cast<int>(npu_cores_.size())) {
        return ToRknnCoreMask(npu_cores_[idx]);
    }
    return ToRknnCoreMask(0);
}

// 启用槽位：优先从 warm 池恢复，否则 clone+Init 新 runtime。
bool ModelCoordinator::EnableSlot(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled_slots_.count(name) && slot_runtimes_.count(name)) {
            return true;
        }
        auto warm_it = warm_runtimes_.find(name);
        if (warm_it != warm_runtimes_.end() && warm_it->second.adapter) {
            slot_runtimes_[name] = std::move(warm_it->second);
            warm_runtimes_.erase(warm_it);
            enabled_slots_.insert(name);
            LogDebug("ModelCoordinator: enabled slot '%s' (from warm pool)", name.c_str());
            return true;
        }
    }

    std::string model_path;
    std::shared_ptr<IModelAdapter> prototype;
    int core_mask = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureModelInPoolUnlocked(name)) {
            LogWarn("ModelCoordinator: cannot enable slot '%s' (no prototype)", name.c_str());
            return false;
        }
        auto& entry = model_pool_[name];
        prototype = entry.prototype;
        model_path = entry.model_path;
        core_mask = CoreMaskForSlot(name);
    }

    auto clone = prototype ? prototype->Clone() : nullptr;
    if (!clone) {
        return false;
    }
    if (clone->Init(model_path, core_mask) != 0) {
        LogWarn("ModelCoordinator: Init failed for slot '%s' path=%s core=0x%x",
                name.c_str(), model_path.c_str(), core_mask);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled_slots_.count(name)) {
            return true;
        }
        slot_runtimes_[name] = {std::move(clone)};
        enabled_slots_.insert(name);
        LogDebug("ModelCoordinator: enabled slot '%s' core_mask=0x%x", name.c_str(), core_mask);
    }
    return true;
}

// 禁用槽位：不销毁 runtime，转入 warm 池以加速下次启用。
void ModelCoordinator::DisableSlot(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_slots_.count(name)) {
        return;
    }
    auto it = slot_runtimes_.find(name);
    if (it != slot_runtimes_.end()) {
        warm_runtimes_[name] = std::move(it->second);
        slot_runtimes_.erase(it);
    }
    enabled_slots_.erase(name);
    LogDebug("ModelCoordinator: disabled slot '%s' (kept warm)", name.c_str());
}

std::vector<std::pair<std::string, std::shared_ptr<IModelAdapter>>>
ModelCoordinator::GetEnabledSlotAdapters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, std::shared_ptr<IModelAdapter>>> out;
    for (const auto& name : OrderedSlotNames()) {
        if (!enabled_slots_.count(name)) {
            continue;
        }
        auto it = slot_runtimes_.find(name);
        if (it != slot_runtimes_.end() && it->second.adapter) {
            out.emplace_back(name, it->second.adapter);
        }
    }
    return out;
}

// 返回可视化用的槽位徽标字符串，例如 "yolo+scrfd"。
std::string ModelCoordinator::GetEnabledSlotsBadge() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    bool first = true;
    for (const auto& name : OrderedSlotNames()) {
        if (!enabled_slots_.count(name)) {
            continue;
        }
        if (!first) {
            oss << "+";
        }
        oss << name;
        first = false;
    }
    return first ? "none" : oss.str();
}

// yolo 与 scrfd 同时启用时，抑制 yolo 的 person 框绘制，避免重复框干扰。
bool ModelCoordinator::ShouldSuppressYoloPersonDraw() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_slots_.count("yolo") > 0 && enabled_slots_.count("scrfd") > 0;
}

// 人脸对话 TTS 活跃时只跳过 yolo 推理，不关闭槽位，避免扰动 scene/门控状态机。
bool ModelCoordinator::ShouldSkipYoloForDialogueTts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tts_visual_throttle_enabled_ && applied_scene_ == CoordinatorScene::Person &&
           enabled_slots_.count("scrfd") > 0 && llm_greeting_.ShouldThrottleVisionForTts();
}

// 场景枚举转文本。
const char* ModelCoordinator::SceneName(CoordinatorScene scene) {
    switch (scene) {
        case CoordinatorScene::Person:
            return "person";
        case CoordinatorScene::Idle:
        default:
            return "idle";
    }
}

// 根据去抖计数推导当前场景（person/idle）。
CoordinatorScene ModelCoordinator::ComputeSceneUnlocked() const {
    if (person_present_count_ >= present_threshold_) {
        return CoordinatorScene::Person;
    }
    return CoordinatorScene::Idle;
}

// 获取 LLM 门控对象引用（供 Pipeline 提交输入与每帧轮询）。
LlmGreeting& ModelCoordinator::GetLlmGreeting() {
    return llm_greeting_;
}

// 设置 person 出现/消失阈值。
void ModelCoordinator::SetSwitchDebounceThresholds(int present_threshold, int absent_threshold) {
    std::lock_guard<std::mutex> lock(mutex_);
    present_threshold_ = present_threshold;
    absent_threshold_ = absent_threshold;
}

// 合并本帧信号到 shared_state（OR/覆盖策略）。
void ModelCoordinator::MergeSignalsUnlocked(const AdapterSignals& signals) {
    if (signals.person_present) {
        shared_state_.person_present = true;
    }
    if (signals.face_detected) {
        shared_state_.face_detected = true;
    }
    if (signals.avg_brightness > 0.0f) {
        shared_state_.avg_brightness = signals.avg_brightness;
    }
    if (!signals.scene_label.empty()) {
        shared_state_.scene_label = signals.scene_label;
    }
}

// 根据当前场景与配置构建目标槽位计划。
ModelCoordinator::SlotPlan ModelCoordinator::BuildSlotPlanUnlocked() {
    SlotPlan plan;
    const CoordinatorScene computed = ComputeSceneUnlocked();
    plan.scene = (scene_dwell_count_ >= scene_dwell_frames_) ? computed : applied_scene_;

    switch (plan.scene) {
        case CoordinatorScene::Person:
            plan.want_scrfd = true;
            plan.want_yolo = true;
            break;
        case CoordinatorScene::Idle:
        default:
            // idle 期保持 yolo 常驻策略，不受 TTS throttle 影响，避免掉到 mode:none。
            plan.want_yolo = yolo_always_on_;
            break;
    }
    // 兜底：若本轮计划会导致无视觉槽，且配置允许 yolo 常驻，则强制保留 yolo。
    if (!plan.want_yolo && !plan.want_scrfd && yolo_always_on_) {
        plan.want_yolo = true;
    }
    return plan;
}

// 执行槽位计划（只做状态对齐，不做去抖计算）。
void ModelCoordinator::ApplySlotPlan(const SlotPlan& plan) {
    auto slot_available = [this](const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        return IsModelAvailableUnlocked(name);
    };

    if (plan.want_yolo && slot_available("yolo")) {
        EnableSlot("yolo");
    } else {
        DisableSlot("yolo");
    }

    if (plan.want_scrfd && slot_available("scrfd")) {
        EnableSlot("scrfd");
    } else {
        DisableSlot("scrfd");
    }
}

// 每帧调度入口：更新去抖状态→应用槽位计划→驱动 LLM 门控。
void ModelCoordinator::UpdateAfterFrame(const AdapterSignals& signals, const cv::Mat& frame) {
    (void)frame;
    SlotPlan plan;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        shared_state_.person_present = false;
        shared_state_.face_detected = false;

        MergeSignalsUnlocked(signals);

        // yolo 跳帧时 scrfd 的 face_detected 同样可证明 person 仍在场。
        const bool person_present = signals.person_present || signals.face_detected;
        // person 去抖计数：出现连续计数 + 消失清零逻辑。
        if (person_present) {
            person_present_count_++;
            person_absent_count_ = 0;
        } else {
            person_absent_count_++;
            if (person_absent_count_ >= absent_threshold_) {
                person_present_count_ = 0;
            }
        }

        const CoordinatorScene computed = ComputeSceneUnlocked();
        current_scene_ = computed;
        shared_state_.scene_label = SceneName(computed);
        // scene 驻留去抖：必须持续达到 scene_dwell_frames 才真正 applied。
        if (computed != pending_scene_) {
            pending_scene_ = computed;
            scene_dwell_count_ = 0;
        } else {
            scene_dwell_count_++;
        }
        if (scene_dwell_count_ >= scene_dwell_frames_) {
            applied_scene_ = computed;
        }

        plan = BuildSlotPlanUnlocked();

        const std::string scene_str = SceneName(computed);
        if (scene_str != last_logged_scene_) {
            LogDebug("ModelCoordinator: scene -> %s (applied=%s dwell=%d/%d)",
                    scene_str.c_str(), SceneName(applied_scene_), scene_dwell_count_,
                    scene_dwell_frames_);
            last_logged_scene_ = scene_str;
        }
        const bool skip_yolo = tts_visual_throttle_enabled_ &&
                               applied_scene_ == CoordinatorScene::Person &&
                               enabled_slots_.count("scrfd") > 0 &&
                               llm_greeting_.ShouldThrottleVisionForTts();
        if (skip_yolo != last_tts_throttle_) {
            LogInfo("ModelCoordinator: tts yolo-skip %s (yolo=%d scrfd=%d)",
                    skip_yolo ? "on" : "off",
                    plan.want_yolo ? 1 : 0,
                    plan.want_scrfd ? 1 : 0);
            last_tts_throttle_ = skip_yolo;
        }
    }

    ApplySlotPlan(plan);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool scrfd_on = enabled_slots_.count("scrfd") > 0;
        llm_greeting_.Update(signals, scrfd_on);
        llm_greeting_.PollDeferred();
    }
}
