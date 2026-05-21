/*
 * platform/model_coordinator.cpp — 方案 A：按需多槽
 */
#include "model_coordinator.h"
#include "../adapters/ppocr/text_region_probe.h"
#include "logging.h"
#include <algorithm>
#include <sstream>

namespace {

// 将 npu_cores 配置项转为 RKNN core_mask（支持 0/1/2 索引或 1/2/4 位掩码）。
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

// 固定槽绘制/推理顺序，与角标拼接顺序一致。
std::vector<std::string> ModelCoordinator::OrderedSlotNames() {
    return {"yolo", "scrfd", "ppocr"};
}

// 登记默认模型与 NPU 配置；EnableSlot 在锁外调用，避免与 EnableSlot 内层 lock 死锁。
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
        LogInfo("ModelCoordinator: slot mode init default='%s' path='%s'",
                default_model_name.c_str(), model_path.c_str());
    }

    if (!EnableSlot(default_model_name)) {
        LogError("ModelCoordinator: failed to enable default slot '%s'", default_model_name.c_str());
        return false;
    }
    return true;
}

bool ModelCoordinator::HasActiveAdapters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !enabled_slots_.empty();
}

void ModelCoordinator::RegisterModel(const std::string& name, std::shared_ptr<IModelAdapter> adapter) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (adapter && !model_pool_.count(name)) {
        model_pool_[name] = {adapter, factory_path_map_.count(name) ? factory_path_map_[name] : ""};
    }
}

void ModelCoordinator::RegisterModel(const std::string& name, std::shared_ptr<IModelAdapter> adapter,
                                     const std::string& model_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (adapter) {
        model_pool_[name] = {adapter, model_path};
        factory_path_map_[name] = model_path;
    }
}

void ModelCoordinator::RegisterFactory(const std::string& name,
                                       std::function<std::shared_ptr<IModelAdapter>()> factory,
                                       const std::string& model_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    factory_map_[name] = std::move(factory);
    factory_path_map_[name] = model_path;
    LogInfo("ModelCoordinator: registered factory for '%s' path='%s'", name.c_str(), model_path.c_str());
}

void ModelCoordinator::SetTextRegionProbe(std::shared_ptr<PpocrTextRegionProbe> probe) {
    std::lock_guard<std::mutex> lock(mutex_);
    text_probe_ = std::move(probe);
}

// 配置 idle 是否常开 YOLO、document 是否关 YOLO、文字探针帧间隔。
void ModelCoordinator::SetSlotOptions(bool yolo_always_on, int text_probe_interval_frames,
                                      bool document_disable_yolo) {
    std::lock_guard<std::mutex> lock(mutex_);
    yolo_always_on_ = yolo_always_on;
    document_disable_yolo_ = document_disable_yolo;
    text_probe_interval_ = text_probe_interval_frames > 0 ? text_probe_interval_frames : 5;
}

// 场景切换后需连续若干帧才改 applied_scene_，减轻槽位抖动。
void ModelCoordinator::SetSceneDwellFrames(int frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    scene_dwell_frames_ = frames > 0 ? frames : 1;
}

// 预加载槽到 warm 池（Init 一次），运行期 Enable 优先复用 warm，避免重复 RKNN Init。
bool ModelCoordinator::WarmupSlot(const std::string& name) {
    if (!EnableSlot(name)) {
        return false;
    }
    DisableSlot(name);
    LogInfo("ModelCoordinator: warmup slot '%s' (in warm pool)", name.c_str());
    return true;
}

// 调用方已持 mutex_：模型是否已登记 prototype 或可 factory 懒加载。
bool ModelCoordinator::IsModelAvailableUnlocked(const std::string& name) const {
    return model_pool_.count(name) > 0 || factory_map_.count(name) > 0;
}

// 调用方已持 mutex_：若仅有 factory 则实例化 prototype 写入 model_pool_。
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

// 调用方已持 mutex_：按槽名映射 system.npu_cores 下标到 RKNN core_mask。
int ModelCoordinator::CoreMaskForSlot(const std::string& slot) const {
    int idx = 0;
    if (slot == "scrfd") {
        idx = 1;
    } else if (slot == "ppocr") {
        idx = 2;
    }
    if (idx < static_cast<int>(npu_cores_.size())) {
        return ToRknnCoreMask(npu_cores_[idx]);
    }
    return ToRknnCoreMask(0);
}

// 启用槽：禁止在已持 mutex_ 时调用；冷启动 RKNN Init 在锁外，避免阻塞 ProbeTextRegion。
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
            LogInfo("ModelCoordinator: enabled slot '%s' (from warm pool)", name.c_str());
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
        LogInfo("ModelCoordinator: enabled slot '%s' core_mask=0x%x", name.c_str(), core_mask);
    }
    return true;
}

// 禁用槽：runtime 移入 warm_runtimes_，保留 RKNN 上下文供下次快速 Enable。
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
    LogInfo("ModelCoordinator: disabled slot '%s' (kept warm)", name.c_str());
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
    if (first) {
        return "none";
    }
    return oss.str();
}

std::string ModelCoordinator::GetCurrentModelName() const {
    return GetEnabledSlotsBadge();
}

std::string ModelCoordinator::GetCurrentScene() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return SceneName(current_scene_);
}

bool ModelCoordinator::ShouldSuppressYoloPersonDraw() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_slots_.count("yolo") > 0 && enabled_slots_.count("scrfd") > 0 &&
           enabled_slots_.count("ppocr") > 0;
}

const char* ModelCoordinator::SceneName(CoordinatorScene scene) {
    switch (scene) {
        case CoordinatorScene::Person:
            return "person";
        case CoordinatorScene::TextOnly:
            return "text_only";
        case CoordinatorScene::Document:
            return "document";
        case CoordinatorScene::Idle:
        default:
            return "idle";
    }
}

CoordinatorScene ModelCoordinator::ComputeSceneUnlocked() const {
    const bool text_stable = text_region_present_count_ >= present_threshold_;
    const bool person_stable = person_present_count_ >= present_threshold_;
    if (text_stable) {
        return CoordinatorScene::Document;
    }
    if (person_stable) {
        return CoordinatorScene::Person;
    }
    return CoordinatorScene::Idle;
}

SharedState& ModelCoordinator::GetSharedState() {
    return shared_state_;
}

LlmGreeting& ModelCoordinator::GetLlmGreeting() {
    return llm_greeting_;
}

void ModelCoordinator::SetSwitchDebounceThresholds(int present_threshold, int absent_threshold,
                                                   int text_absent_threshold) {
    std::lock_guard<std::mutex> lock(mutex_);
    present_threshold_ = present_threshold;
    absent_threshold_ = absent_threshold;
    text_absent_threshold_ = text_absent_threshold;
}

void ModelCoordinator::SetToPpocrMode(const std::string& mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode == "off" || mode == "text" || mode == "frames") {
        to_ppocr_mode_ = mode;
    } else {
        to_ppocr_mode_ = "text";
    }
}

void ModelCoordinator::SetPpocrBackPolicy(bool auto_disable_ppocr_on_no_text, int min_ppocr_frames,
                                          bool back_on_no_text) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto_disable_ppocr_ = auto_disable_ppocr_on_no_text;
    min_ppocr_frames_ = min_ppocr_frames > 0 ? min_ppocr_frames : 90;
    back_on_no_text_ = back_on_no_text;
}

// 按 text_probe_interval_ 节流；PpocrTextRegionProbe::Detect 在锁外，避免与 UpdateAfterFrame 死锁。
bool ModelCoordinator::ProbeTextRegion(const cv::Mat& frame, AdapterSignals& in_out_signals) {
    if (!text_probe_) {
        return false;
    }
    bool use_cached = false;
    bool cached = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (++text_probe_frame_counter_ < text_probe_interval_) {
            use_cached = true;
            cached = shared_state_.text_region_present;
        } else {
            text_probe_frame_counter_ = 0;
        }
    }
    if (use_cached) {
        in_out_signals.text_region_present = cached;
        return cached;
    }

    const bool has_text = text_probe_->Detect(frame);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        in_out_signals.text_region_present = has_text;
        shared_state_.text_region_present = has_text;
    }
    return has_text;
}

// 调用方已持 mutex_：合并本帧各槽信号到 shared_state_（OR 语义）。
void ModelCoordinator::MergeSignalsUnlocked(const AdapterSignals& signals) {
    if (signals.person_present) {
        shared_state_.person_present = true;
    }
    if (signals.text_region_present) {
        shared_state_.text_region_present = true;
    }
    if (signals.face_detected) {
        shared_state_.face_detected = true;
    }
    if (signals.text_detected) {
        shared_state_.text_detected = true;
    }
    if (signals.avg_brightness > 0.0f) {
        shared_state_.avg_brightness = signals.avg_brightness;
    }
    if (!signals.scene_label.empty()) {
        shared_state_.scene_label = signals.scene_label;
    }
    last_signals_ = signals;
}

// 调用方已持 mutex_：按 applied_scene_（dwell 后）生成 want_*，见 docs/多模型并行问题.md。
ModelCoordinator::SlotPlan ModelCoordinator::BuildSlotPlanUnlocked() {
    SlotPlan plan;
    const CoordinatorScene computed = ComputeSceneUnlocked();
    plan.scene = (scene_dwell_count_ >= scene_dwell_frames_) ? computed : applied_scene_;

    switch (plan.scene) {
        case CoordinatorScene::Document:
            plan.want_scrfd = true;
            plan.want_yolo = !document_disable_yolo_;
            break;
        case CoordinatorScene::TextOnly:
            plan.want_yolo = yolo_always_on_;
            break;
        case CoordinatorScene::Person:
            plan.want_scrfd = true;
            plan.want_yolo = true;
            break;
        case CoordinatorScene::Idle:
        default:
            plan.want_yolo = yolo_always_on_;
            break;
    }

    if (to_ppocr_mode_ != "off" && plan.scene == CoordinatorScene::Document) {
        if (to_ppocr_mode_ == "text") {
            plan.want_ppocr = text_region_present_count_ >= present_threshold_;
        } else if (to_ppocr_mode_ == "frames") {
            ppocr_lab_frame_count_++;
            plan.want_ppocr = ppocr_lab_frame_count_ >= present_threshold_;
        }
    }
    return plan;
}

// 按计划启停槽；内部调用 EnableSlot/DisableSlot（均会自行加锁，且 Init 在锁外）。
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

    if (plan.want_ppocr && slot_available("ppocr")) {
        EnableSlot("ppocr");
    } else {
        DisableSlot("ppocr");
        std::lock_guard<std::mutex> lock(mutex_);
        ppocr_lab_frame_count_ = 0;
    }
}

// 主线程每帧显示后：去抖计数、场景 dwell、锁外 ApplySlotPlan，再更新问候语。
void ModelCoordinator::UpdateAfterFrame(const AdapterSignals& signals, const cv::Mat& frame) {
    (void)frame;
    SlotPlan plan;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        shared_state_.person_present = false;
        shared_state_.face_detected = false;
        shared_state_.text_detected = false;

        MergeSignalsUnlocked(signals);

        if (signals.person_present) {
            person_present_count_++;
            person_absent_count_ = 0;
        } else {
            person_absent_count_++;
            if (person_absent_count_ >= absent_threshold_) {
                person_present_count_ = 0;
            }
        }

        if (signals.text_region_present) {
            text_region_present_count_++;
            text_region_absent_count_ = 0;
        } else {
            text_region_absent_count_++;
            if (text_region_absent_count_ >= absent_threshold_) {
                text_region_present_count_ = 0;
            }
        }

        const CoordinatorScene computed = ComputeSceneUnlocked();
        current_scene_ = computed;
        shared_state_.scene_label = SceneName(computed);
        if (computed != pending_scene_) {
            pending_scene_ = computed;
            scene_dwell_count_ = 0;
        } else {
            scene_dwell_count_++;
        }
        if (scene_dwell_count_ >= scene_dwell_frames_) {
            applied_scene_ = computed;
        }

        if (enabled_slots_.count("scrfd")) {
            if (signals.face_detected) {
                face_absent_count_ = 0;
            } else {
                face_absent_count_++;
            }
        } else {
            face_absent_count_ = 0;
        }

        const bool in_document = (applied_scene_ == CoordinatorScene::Document);
        if (enabled_slots_.count("ppocr")) {
            ppocr_active_frames_++;
            if (signals.text_detected) {
                text_absent_count_ = 0;
            } else {
                text_absent_count_++;
            }
            if (!in_document && auto_disable_ppocr_ && back_on_no_text_ &&
                ppocr_active_frames_ >= min_ppocr_frames_ &&
                text_absent_count_ >= text_absent_threshold_) {
                text_region_present_count_ = 0;
                ppocr_active_frames_ = 0;
                text_absent_count_ = 0;
            }
        } else {
            ppocr_active_frames_ = 0;
            text_absent_count_ = 0;
        }

        plan = BuildSlotPlanUnlocked();

        const std::string scene_str = SceneName(computed);
        if (scene_str != last_logged_scene_) {
            LogInfo("ModelCoordinator: scene -> %s (applied=%s dwell=%d/%d)",
                    scene_str.c_str(), SceneName(applied_scene_), scene_dwell_count_,
                    scene_dwell_frames_);
            last_logged_scene_ = scene_str;
        }
    }

    ApplySlotPlan(plan);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool scrfd_on = enabled_slots_.count("scrfd") > 0;
        llm_greeting_.Update(signals, scrfd_on);
    }
}
