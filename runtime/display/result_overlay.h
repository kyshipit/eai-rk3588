/*
 * display/result_overlay.h
 * 将适配器 Postprocess 行文本绘制到帧上。
 */
#pragma once

#include <opencv2/opencv.hpp>
#include <string>

class ResultOverlay {
public:
    // 解析并绘制检测结果行文本（label x1 y1 x2 y2 score ...）。
    void Apply(cv::Mat& frame, const std::string& result_json,
               bool suppress_yolo_person = false) const;
    // 绘制当前启用模型徽标。
    void DrawModelBadge(cv::Mat& frame, const std::string& model_name) const;
    // 绘制底部提示条。
    void DrawGreetingBanner(cv::Mat& frame, const std::string& text) const;
};
