/*
 * capture/frame_transform.h
 * 帧旋转与合法性校验（与采集设备无关的图像变换）。
 */
#pragma once

#include <opencv2/opencv.hpp>
#include <string>

class FrameTransform {
public:
    // rotate: none/ccw90/cw90/180（兼容 0/90cw）。
    explicit FrameTransform(const std::string& rotate = "ccw90");

    // 对输入帧执行旋转变换。
    void Apply(cv::Mat& frame) const;
    // 校验帧是否可进入后续推理链路。
    bool Validate(const cv::Mat& frame, int frame_id) const;

private:
    int rotate_mode_ = 1;  // 0=none 1=ccw90 2=cw90 3=180
};

// 将配置字符串解析为内部旋转模式枚举。
int ParseInputRotateMode(const std::string& rotate);
