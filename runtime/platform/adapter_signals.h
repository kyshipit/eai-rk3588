/*
 * platform/adapter_signals.h
 *
 * 各适配器在 GetAdapterSignals() 中填写；ModelCoordinator 合并进 SharedState。
 */
#pragma once

#include <string>

struct AdapterSignals {
    // 是否检测到人（主要由 yolo 填充）。
    bool person_present = false;
    // 是否检测到人脸（主要由 scrfd 填充）。
    bool face_detected = false;
    // 场景亮度估计（可选信号）。
    float avg_brightness = 0.0f;
    // 适配器自定义场景标签（可选）。
    std::string scene_label;
};
