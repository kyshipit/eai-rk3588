/*
 * adapters/ppocr/text_region_probe.h
 *
 * YOLO 哨兵阶段用 PPOCR det 探测文字区，置位 text_region_present（方案 A）。
 */
#pragma once

#include <mutex>
#include <string>
#include <opencv2/opencv.hpp>

#include "ppocr_system.h"

class PpocrTextRegionProbe {
public:
    PpocrTextRegionProbe() = default;
    ~PpocrTextRegionProbe();

    int Init(const std::string& det_path, int npu_core_mask, float det_threshold, float box_threshold,
             int min_infer_width);
    void Release();

    bool Detect(const cv::Mat& frame_bgr);
    bool LastHasTextRegion() const { return last_has_text_; }

private:
    bool PrepareRgb(const cv::Mat& frame_bgr);

    std::mutex mutex_;
    bool initialized_ = false;
    bool last_has_text_ = false;
    int min_infer_width_ = 960;
    ppocr_rknn_context_t det_ctx_;
    ppocr_det_postprocess_params det_params_;
    image_buffer_t src_image_;
    cv::Mat frame_rgb_;
    cv::Mat frame_infer_;
};
