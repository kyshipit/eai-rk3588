/*
 * platform/shared_state.h
 *
 * 跨帧合并后的决策状态（去抖后由 ModelCoordinator 读写）。
 */
#pragma once

#include <string>

struct SharedState {
    // 去抖后的“有人”状态。
    bool person_present = false;
    // 去抖后的“有人脸”状态。
    bool face_detected = false;
    // 融合后的亮度信号。
    float avg_brightness = 0.0f;
    // 当前场景标签（如 person/idle）。
    std::string scene_label;
};
