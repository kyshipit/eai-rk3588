/*
 * adapters/scrfd/scrfd_adapter.h — SCRFD 人脸检测 RKNN 适配器
 */
#pragma once

#include <memory>
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

#include "engine/adapter_interface.h"
#include "scrfd_context.h"
#include "scrfd_postprocess.h"

class ScrfdAdapter : public IModelAdapter {
public:
    // 构造：初始化上下文与默认阈值。
    ScrfdAdapter();
    // 析构：释放 RKNN 资源。
    ~ScrfdAdapter() override;

    // 加载 SCRFD 模型并绑定 NPU 核。
    int Init(const std::string& model_path, int npu_core_mask) override;
    // 预处理：letterbox 到 640x640，输出模型输入缓冲指针。
    uint8_t* Preprocess(const cv::Mat& frame, int& out_size) override;
    // 推理：执行 rknn_run 并拉取输出张量。
    int Inference(std::shared_ptr<void>& model_output) override;
    // 后处理：解码人脸框+关键点并序列化为 overlay 文本。
    std::string Postprocess(const std::shared_ptr<void>& model_output) override;
    // 克隆：供多线程槽位 runtime 使用。
    std::shared_ptr<IModelAdapter> Clone() const override;
    // 输出适配器信号（当前主要提供 face_detected）。
    AdapterSignals GetAdapterSignals() const override;

    // 设置后处理阈值。
    void SetThresholds(float conf_threshold, float nms_threshold);

    // 获取最近一帧的人脸结果（调试/扩展用途）。
    const std::vector<ScrfdFaceBox>& GetLastFaces() const;

private:
    struct RknnOutputHolder {
        std::vector<rknn_output> outputs;
        rknn_context ctx = 0;
    };

    scrfd_app_context_t app_ctx_;
    ScrfdLetterbox letterbox_;
    std::vector<ScrfdFaceBox> last_faces_;
    std::vector<uint8_t> input_buf_;
    float conf_threshold_ = 0.5f;
    float nms_threshold_ = 0.5f;
    bool initialized_ = false;
};
