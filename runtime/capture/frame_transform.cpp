/*
 * capture/frame_transform.cpp
 *
 * 帧几何处理实现：
 * - ParseInputRotateMode：解析配置中的旋转枚举。
 * - Apply：按模式旋转图像。
 * - Validate：在进入推理前做基础图像合法性检查。
 */
#include "frame_transform.h"
#include "platform/logging.h"

int ParseInputRotateMode(const std::string& rotate) {
    if (rotate == "none" || rotate == "0") {
        return 0;
    }
    if (rotate == "cw90" || rotate == "90cw") {
        return 2;
    }
    if (rotate == "180") {
        return 3;
    }
    return 1;
}

FrameTransform::FrameTransform(const std::string& rotate)
    : rotate_mode_(ParseInputRotateMode(rotate)) {}

void FrameTransform::Apply(cv::Mat& frame) const {
    if (frame.empty() || rotate_mode_ == 0) {
        return;
    }
    cv::Mat rotated;
    switch (rotate_mode_) {
        case 2:
            cv::rotate(frame, rotated, cv::ROTATE_90_CLOCKWISE);
            break;
        case 3:
            cv::rotate(frame, rotated, cv::ROTATE_180);
            break;
        case 1:
        default:
            cv::rotate(frame, rotated, cv::ROTATE_90_COUNTERCLOCKWISE);
            break;
    }
    frame = rotated;
}

bool FrameTransform::Validate(const cv::Mat& frame, int frame_id) const {
    // 这里做的是推理前底线校验，错误直接打日志并丢帧，避免传入适配器崩溃。
    if (frame.empty() || frame.data == nullptr) {
        LogError("FrameTransform: frame %d is empty", frame_id);
        return false;
    }
    if (frame.cols < 32 || frame.rows < 32) {
        LogError("FrameTransform: frame %d too small (%dx%d)", frame_id, frame.cols, frame.rows);
        return false;
    }
    if (frame.channels() != 3 || frame.type() != CV_8UC3) {
        LogError("FrameTransform: frame %d unsupported type (channels=%d type=0x%x)",
                 frame_id, frame.channels(), frame.type());
        return false;
    }
    return true;
}
