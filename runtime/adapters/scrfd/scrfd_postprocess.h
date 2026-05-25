/*
 * adapters/scrfd/scrfd_postprocess.h — SCRFD 解码 + NMS（对齐 adapters/scrfd/main.py）
 */
#pragma once

#include "scrfd_context.h"
#include <opencv2/opencv.hpp>
#include <vector>

struct ScrfdLetterbox {
    // 原图宽高。
    int orig_w = 0;
    int orig_h = 0;
    // resize 后（padding 前）的宽高。
    int new_w = 0;
    int new_h = 0;
    // 左右/上下填充。
    int pad_w = 0;
    int pad_h = 0;
};

struct ScrfdFaceBox {
    // 人脸框坐标（xyxy）。
    float x1 = 0.f;
    float y1 = 0.f;
    float x2 = 0.f;
    float y2 = 0.f;
    // 置信度。
    float score = 0.f;
    // 5 个关键点。
    cv::Point2f kps[5];
};

// SCRFD 后处理：解码 score/bbox/kps，执行 NMS，输出人脸框结果。
int scrfd_postprocess(scrfd_app_context_t* ctx,
                      const rknn_output* outputs,
                      const ScrfdLetterbox& letterbox,
                      float conf_threshold,
                      float nms_threshold,
                      std::vector<ScrfdFaceBox>& faces_out);
