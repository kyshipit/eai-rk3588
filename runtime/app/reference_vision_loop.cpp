/*
 * app/reference_vision_loop.cpp — 参考应用显示与门控驱动
 */
#include "reference_vision_loop.h"

#include "platform/logging.h"

ReferenceVisionLoop::ReferenceVisionLoop(ModelCoordinator& coordinator,
                                         FrameTransform& frame_transform,
                                         ResultOverlay& overlay,
                                         IDisplaySink& display)
    : coordinator_(coordinator),
      frame_transform_(frame_transform),
      overlay_(overlay),
      display_(display) {}

// 准备 OpenCV 显示后端。
void ReferenceVisionLoop::Prepare() {
    display_.Prepare();
}

// 释放显示资源。
void ReferenceVisionLoop::Shutdown() {
    display_.Shutdown();
}

// 更新场景机、绘制检测层并刷新预览；ESC 由 PumpIdle/此处统一处理。
bool ReferenceVisionLoop::OnInferenceTask(InferenceTask& task) {
    if (task.frame_id == -1) {
        return false;
    }
    if (!frame_transform_.Validate(task.original_frame, task.frame_id)) {
        return true;
    }

    coordinator_.UpdateAfterFrame(task.merged_signals, task.original_frame);

    const std::string badge = coordinator_.GetEnabledSlotsBadge();
    if (badge != last_badge_) {
        LogDebug("ReferenceVisionLoop: enabled slots -> %s (frame_id=%d)",
                 badge.c_str(), task.frame_id);
        last_badge_ = badge;
    }

    const bool suppress_yolo_person = coordinator_.ShouldSuppressYoloPersonDraw();
    for (const auto& layer : task.slot_results) {
        const bool suppress = (layer.slot == "yolo") && suppress_yolo_person;
        overlay_.Apply(task.original_frame, layer.result_json, suppress);
    }
    overlay_.DrawModelBadge(task.original_frame, badge);

    cv::Mat display =
        task.original_frame.isContinuous() ? task.original_frame : task.original_frame.clone();
    display_.Show(display);

    if (task.frame_id == 0) {
        LogInfo("ReferenceVisionLoop: first frame displayed id=%d", task.frame_id);
    }
    return true;
}

// 非阻塞轮询 ESC；并执行 app 注入的 stdin 等 idle 逻辑。
bool ReferenceVisionLoop::PumpIdle(const std::function<void()>& idle_handler) {
    const int key = display_.PollKey(1);
    if (key == 27) {
        return false;
    }
    if (idle_handler) {
        idle_handler();
    }
    return true;
}
