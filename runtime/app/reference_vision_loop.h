/*
 * app/reference_vision_loop.h
 *
 * 参考应用视觉主循环：协调器更新、检测框叠加、OpenCV 预览与 ESC 退出。
 * 与 engine/Pipeline 解耦，Pipeline 仅投递 InferenceTask。
 */
#pragma once

#include <functional>
#include <string>

#include <opencv2/opencv.hpp>

#include "engine/pipeline.h"
#include "platform/model_coordinator.h"
#include "capture/frame_transform.h"
#include "display/display_sink.h"
#include "display/result_overlay.h"

class ReferenceVisionLoop {
public:
    ReferenceVisionLoop(ModelCoordinator& coordinator,
                        FrameTransform& frame_transform,
                        ResultOverlay& overlay,
                        IDisplaySink& display);

    // 创建预览窗口等资源。
    void Prepare();
    // 关闭预览窗口。
    void Shutdown();

    // Pipeline 每帧后处理回调；返回 false 表示收到 quit（含 frame_id=-1）。
    bool OnInferenceTask(InferenceTask& task);
    // 队列空时泵送按键与 idle（如 ConsoleUi）；返回 false 表示 ESC 请求退出。
    bool PumpIdle(const std::function<void()>& idle_handler);

private:
    ModelCoordinator& coordinator_;
    FrameTransform& frame_transform_;
    ResultOverlay& overlay_;
    IDisplaySink& display_;
    std::string last_badge_;
};
