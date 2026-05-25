/*
 * adapters/scrfd/scrfd_context.h — SCRFD RKNN 上下文（独立于 YOLO 类型名）
 */
#pragma once

#include "rknn_api.h"

typedef struct {
    rknn_context rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr* input_attrs;
    rknn_tensor_attr* output_attrs;
    // 模型输入形状信息（供预处理与后处理使用）。
    int model_channel;
    int model_width;
    int model_height;
} scrfd_app_context_t;
