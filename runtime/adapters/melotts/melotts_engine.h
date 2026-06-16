// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef _MELOTTS_ENGINE_H_
#define _MELOTTS_ENGINE_H_

#include <iostream>
#include <vector>
#include <string>
#include "rknn_api.h"
#include "melotts_process.h"

// 单个 MeloTTS RKNN 子模型（encoder 或 decoder）的运行时上下文。
typedef struct
{
    rknn_context rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr *input_attrs;
    rknn_tensor_attr *output_attrs;
    rknn_input_range *shape_range;
} melotts_rknn_context_t;

// encoder + decoder 成对上下文。
typedef struct
{
    melotts_rknn_context_t encoder_context;
    melotts_rknn_context_t decoder_context;
} rknn_melotts_context_t;

// 从文件加载 RKNN 并缓存 tensor 属性；失败返回非 0。
int init_melotts_model(const char *model_path, melotts_rknn_context_t *app_ctx);
// 释放 RKNN 与 malloc 的 attr 缓冲。
int release_melotts_model(melotts_rknn_context_t *app_ctx);

// 运行 encoder：音素序列 → duration / prior 等中间张量。
int inference_encoder_model(melotts_rknn_context_t *app_ctx, std::vector<int64_t> &x,
    int64_t x_lengths, int64_t speaker_id, std::vector<int64_t> &tones, std::vector<int64_t> &lang_ids,
    std::vector<float> &ja_bert, std::vector<float> &logw, std::vector<float> &x_mask,
    std::vector<float> &g, std::vector<float> &m_p, std::vector<float> &logs_p);

// 运行 decoder：attention 与 prior → 波形 PCM float。
int inference_decoder_model(melotts_rknn_context_t *app_ctx, std::vector<float> &attn, std::vector<float> &y_mask, std::vector<float> &g, 
        std::vector<float> &m_p, std::vector<float> &logs_p, int &predicted_lengths_max_real, std::vector<float> &output_wav_data);

// 端到端：phones/tones/lang → encoder → middle_process → decoder。
int inference_melotts_model(rknn_melotts_context_t *app_ctx, std::vector<int64_t> &phones,
    int64_t phone_len, std::vector<int64_t> &tones, std::vector<int64_t> &lang_ids,
    int64_t speaker_id, float speed, bool disable_bert, std::vector<float> &output_wav_data);

#endif  // _MELOTTS_ENGINE_H_
