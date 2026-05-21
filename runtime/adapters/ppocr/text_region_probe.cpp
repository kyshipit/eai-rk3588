/*
 * adapters/ppocr/text_region_probe.cpp
 */
#include "text_region_probe.h"
#include "rknn_api.h"
#include "platform/logging.h"
#include <cstring>

PpocrTextRegionProbe::~PpocrTextRegionProbe() {
    Release();
}

int PpocrTextRegionProbe::Init(const std::string& det_path, int npu_core_mask, float det_threshold,
                               float box_threshold, int min_infer_width) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return 0;
    }
    memset(&det_ctx_, 0, sizeof(det_ctx_));
    memset(&det_params_, 0, sizeof(det_params_));
    memset(&src_image_, 0, sizeof(src_image_));
    det_params_.threshold = det_threshold;
    det_params_.box_threshold = box_threshold;
    det_params_.use_dilate = false;
    det_params_.db_score_mode = const_cast<char*>("slow");
    det_params_.db_box_type = const_cast<char*>("poly");
    det_params_.db_unclip_ratio = 1.5f;
    min_infer_width_ = min_infer_width > 0 ? min_infer_width : 960;

    int ret = init_ppocr_model(det_path.c_str(), &det_ctx_);
    if (ret != 0) {
        LogWarn("PpocrTextRegionProbe: init det failed path=%s ret=%d", det_path.c_str(), ret);
        return ret;
    }
    rknn_set_core_mask(det_ctx_.rknn_ctx, static_cast<rknn_core_mask>(npu_core_mask));
    initialized_ = true;
    LogInfo("PpocrTextRegionProbe: init ok det_th=%.2f box_th=%.2f min_w=%d",
            det_threshold, box_threshold, min_infer_width_);
    return 0;
}

void PpocrTextRegionProbe::Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        release_ppocr_model(&det_ctx_);
        initialized_ = false;
    }
    last_has_text_ = false;
}

bool PpocrTextRegionProbe::PrepareRgb(const cv::Mat& frame_bgr) {
    if (frame_bgr.empty()) {
        return false;
    }
    if (frame_bgr.channels() == 3) {
        cv::cvtColor(frame_bgr, frame_rgb_, cv::COLOR_BGR2RGB);
    } else if (frame_bgr.channels() == 4) {
        cv::cvtColor(frame_bgr, frame_rgb_, cv::COLOR_BGRA2RGB);
    } else {
        frame_rgb_ = frame_bgr.clone();
    }
    const cv::Mat* infer_src = &frame_rgb_;
    if (frame_rgb_.cols < min_infer_width_) {
        const float scale = static_cast<float>(min_infer_width_) / static_cast<float>(frame_rgb_.cols);
        cv::resize(frame_rgb_, frame_infer_, cv::Size(), scale, scale, cv::INTER_LINEAR);
        infer_src = &frame_infer_;
    } else {
        frame_infer_.release();
    }
    src_image_.height = infer_src->rows;
    src_image_.width = infer_src->cols;
    src_image_.width_stride = static_cast<int>(infer_src->step[0]);
    src_image_.virt_addr = infer_src->data;
    src_image_.format = IMAGE_FORMAT_RGB888;
    src_image_.size = static_cast<int>(infer_src->total() * infer_src->elemSize());
    return src_image_.virt_addr != nullptr && src_image_.size > 0;
}

bool PpocrTextRegionProbe::Detect(const cv::Mat& frame_bgr) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_has_text_ = false;
    if (!initialized_ || !PrepareRgb(frame_bgr)) {
        return false;
    }
    ppocr_det_result det_results;
    memset(&det_results, 0, sizeof(det_results));
    int ret = inference_ppocr_det_model(&det_ctx_, &src_image_, &det_params_, &det_results);
    if (ret == 0 && det_results.count > 0) {
        last_has_text_ = true;
    }
    return last_has_text_;
}
