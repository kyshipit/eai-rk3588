/*
 * display/display_sink.h
 * 显示输出抽象（imshow / headless）。
 */
#pragma once

#include <memory>
#include <opencv2/opencv.hpp>

#include "display_layout.h"

class IDisplaySink {
public:
    virtual ~IDisplaySink() = default;
    // 初始化显示资源（窗口创建前置步骤）。
    virtual void Prepare() = 0;
    // 显示一帧图像。
    virtual void Show(const cv::Mat& frame) = 0;
    // 轮询键盘输入。
    virtual int PollKey(int delay_ms) = 0;
    // 释放显示资源。
    virtual void Shutdown() = 0;
};

// 根据配置创建 OpenCV 或空实现显示后端。
std::unique_ptr<IDisplaySink> CreateOpenCVDisplaySink(const DisplayWindowConfig& cfg,
                                                      const char* window_name = "EdgeAI Platform");
