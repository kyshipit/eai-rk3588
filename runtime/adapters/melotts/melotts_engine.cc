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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

#include "melotts_process.h"
#include "melotts_engine.h"
#include "easy_timer.h"
#include "file_utils.h"
#include "platform/logging.h"

// 将 Melo 各阶段耗时写入 debug 日志，避免默认 info 下刷屏 stdout。
static void LogMeloPhaseTime(TIMER& timer, const char* phase) {
    LogDebug("MeloTTS: %s use: %.3f ms", phase, timer.get_time());
}

// 调试打印 RKNN 动态 shape 范围。
static void dump_input_dynamic_range(const char* model_tag, rknn_input_range *dyn_range)
{
    std::string range_str = "";
    for (int n = 0; n < dyn_range->shape_number; ++n)
    {
        range_str += n == 0 ? "[" : ",[";
        range_str += dyn_range->n_dims < 1 ? "" : std::to_string(dyn_range->dyn_range[n][0]);
        for (int i = 1; i < dyn_range->n_dims; ++i)
        {
            range_str += ", " + std::to_string(dyn_range->dyn_range[n][i]);
        }
        range_str += "]";
    }

    LogInfo("%s: input_range index=%d name=%s shape_number=%d range=[%s] fmt=%s", model_tag,
            dyn_range->index, dyn_range->name, dyn_range->shape_number, range_str.c_str(), get_format_string(dyn_range->fmt));
}

// 调试打印单个 tensor 的维度与量化信息。
static void dump_tensor_attr(const char* model_tag, rknn_tensor_attr *attr)
{
    char dims_str[100];
    char temp_str[100];
    memset(dims_str, 0, sizeof(dims_str));
    for (int i = 0; i < attr->n_dims; i++)
    {
        strcpy(temp_str, dims_str);
        if (i == attr->n_dims - 1)
        {
            sprintf(dims_str, "%s%d", temp_str, attr->dims[i]);
        }
        else
        {
            sprintf(dims_str, "%s%d, ", temp_str, attr->dims[i]);
        }
    }

    LogInfo("%s: index=%d name=%s n_dims=%d dims=[%s] n_elems=%d size=%d fmt=%s type=%s qnt_type=%s zp=%d scale=%f",
            model_tag, attr->index, attr->name, attr->n_dims, dims_str, attr->n_elems, attr->size, get_format_string(attr->fmt),
            get_type_string(attr->type), get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

// 加载 RKNN 模型文件并查询 IO tensor 属性。
int init_melotts_model(const char *model_path, melotts_rknn_context_t *app_ctx)
{
    int ret;
    int model_len = 0;
    rknn_context ctx = 0;

    ret = rknn_init(&ctx, (char *)model_path, model_len, 0, NULL);
    if (ret < 0)
    {
        LogError("MeloTTS: model=%s rknn_init fail ret=%d", model_path, ret);
        return -1;
    }

    // Get Model Input Output Number
    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC)
    {
        LogError("MeloTTS: model=%s rknn_query RKNN_QUERY_IN_OUT_NUM fail ret=%d", model_path, ret);
        return -1;
    }
    LogInfo("MeloTTS: model=%s n_input=%d n_output=%d", model_path, io_num.n_input, io_num.n_output);

    // Get Model Input Info
    LogInfo("MeloTTS: model=%s input tensors:", model_path);
    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++)
    {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            LogError("MeloTTS: model=%s rknn_query RKNN_QUERY_INPUT_ATTR fail ret=%d", model_path, ret);
            return -1;
        }
        dump_tensor_attr(model_path, &(input_attrs[i]));
    }

    // Get Model Output Info
    LogInfo("MeloTTS: model=%s output tensors:", model_path);
    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++)
    {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            LogError("MeloTTS: model=%s rknn_query RKNN_QUERY_OUTPUT_ATTR fail ret=%d", model_path, ret);
            return -1;
        }
        dump_tensor_attr(model_path, &(output_attrs[i]));
    }

    // Set to context
    app_ctx->rknn_ctx = ctx;
    app_ctx->io_num = io_num;
    app_ctx->input_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->input_attrs, input_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
    app_ctx->output_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->output_attrs, output_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

    app_ctx->shape_range = (rknn_input_range *)malloc(io_num.n_input * sizeof(rknn_input_range));
    memset(app_ctx->shape_range, 0, io_num.n_input * sizeof(rknn_input_range));
    for (uint32_t i = 0; i < io_num.n_input; ++i) {
        app_ctx->shape_range[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_DYNAMIC_RANGE, &(app_ctx->shape_range[i]),
                         sizeof(rknn_input_range));
        if (ret == RKNN_SUCC && app_ctx->shape_range[i].shape_number > 0) {
            dump_input_dynamic_range(model_path, &(app_ctx->shape_range[i]));
        }
    }

    return 0;
}

// 释放 RKNN 上下文与 tensor 属性缓存。
int release_melotts_model(melotts_rknn_context_t *app_ctx)
{
    if (app_ctx->input_attrs != NULL)
    {
        free(app_ctx->input_attrs);
        app_ctx->input_attrs = NULL;
    }
    if (app_ctx->output_attrs != NULL)
    {
        free(app_ctx->output_attrs);
        app_ctx->output_attrs = NULL;
    }

    if (app_ctx->shape_range != NULL)
    {
        free(app_ctx->shape_range);
        app_ctx->shape_range = NULL;
    }
    if (app_ctx->rknn_ctx != 0)
    {
        rknn_destroy(app_ctx->rknn_ctx);
        app_ctx->rknn_ctx = 0;
    }
    return 0;
}

// 填充 encoder 输入并 rknn_run，取出 logw / prior 等供 middle_process 使用。
int inference_encoder_model(melotts_rknn_context_t *app_ctx, std::vector<int64_t> &x,
    int64_t x_lengths, int64_t speaker_id, std::vector<int64_t> &tones, std::vector<int64_t> &lang_ids,
    std::vector<float> &ja_bert, std::vector<float> &logw, std::vector<float> &x_mask,
    std::vector<float> &g, std::vector<float> &m_p, std::vector<float> &logs_p)
{
    int ret;
    int n_input = 8;
    int n_output = 5;
    rknn_input inputs[n_input];
    rknn_output outputs[n_output];

    memset(inputs, 0, sizeof(inputs));
    memset(outputs, 0, sizeof(outputs));

    float sdp_ratio = SDP_RATIO;
    float noise_scale_w = NOISE_SCALE_W;

    // Set Input Data
    //['x', 'x_lengths', 'sid', 'tone', 'lang_ids', 'ja_bert', 'noise_scale_w', 'sdp_ratio']
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_INT64;
    inputs[0].fmt = RKNN_TENSOR_UNDEFINED;
    inputs[0].size = INPUT_SIZE * sizeof(int64_t);
    inputs[0].buf = (int64_t *)malloc(inputs[0].size);
    memcpy(inputs[0].buf, x.data(), inputs[0].size);

    inputs[1].index = 1;
    inputs[1].type = RKNN_TENSOR_INT64;
    inputs[1].size = 1 * sizeof(int64_t);
    inputs[1].fmt = RKNN_TENSOR_UNDEFINED;
    inputs[1].buf = (int64_t *)malloc(inputs[1].size);
    memcpy(inputs[1].buf, &x_lengths, inputs[1].size);

    inputs[2].index = 2;
    inputs[2].type = RKNN_TENSOR_INT64;
    inputs[2].size = 1 * sizeof(int64_t);
    inputs[2].fmt = RKNN_TENSOR_UNDEFINED;
    inputs[2].buf = (int64_t *)malloc(inputs[2].size);
    memcpy(inputs[2].buf, &speaker_id, inputs[2].size);

    inputs[3].index = 3;
    inputs[3].type = RKNN_TENSOR_INT64;
    inputs[3].size = INPUT_SIZE  * sizeof(int64_t);
    inputs[3].fmt = RKNN_TENSOR_UNDEFINED;
    inputs[3].buf = (int64_t *)malloc(inputs[3].size);
    memcpy(inputs[3].buf, tones.data(), inputs[3].size);

    inputs[4].index = 4;
    inputs[4].type = RKNN_TENSOR_INT64;
    inputs[4].size = INPUT_SIZE * sizeof(int64_t);
    inputs[4].fmt = RKNN_TENSOR_UNDEFINED;
    inputs[4].buf = (int64_t *)malloc(inputs[4].size);
    memcpy(inputs[4].buf, lang_ids.data(), inputs[4].size);

    inputs[5].index = 5;
    inputs[5].type = RKNN_TENSOR_FLOAT32;
    inputs[5].size = 1 * 768 * 256 * sizeof(float);
    inputs[5].buf = (float *)malloc(inputs[5].size);
    memcpy(inputs[5].buf, ja_bert.data(), inputs[5].size);

    inputs[6].index = 6;
    inputs[6].type = RKNN_TENSOR_FLOAT32;
    inputs[6].size = 1  * sizeof(float);
    inputs[6].buf = (float *)malloc(inputs[6].size);
    memcpy(inputs[6].buf, &noise_scale_w, inputs[6].size);

    inputs[7].index = 7;
    inputs[7].type = RKNN_TENSOR_FLOAT32;
    inputs[7].size = 1  * sizeof(float);
    inputs[7].buf = (float *)malloc(inputs[7].size);
    memcpy(inputs[7].buf, &sdp_ratio, inputs[7].size);

    ret = rknn_inputs_set(app_ctx->rknn_ctx, n_input, inputs);
    if (ret < 0)
    {
        LogError("MeloTTS: rknn_inputs_set failed ret=%d", ret);
        goto out;
    }

    // Run
    ret = rknn_run(app_ctx->rknn_ctx, NULL);
    if (ret < 0)
    {
        LogError("MeloTTS: rknn_run failed ret=%d", ret);
        goto out;
    }

    // Get Output
    //["logw", "x_mask", "g", "m_p", "logs_p"]
    for (int i = 0; i < n_output; i++)
    {
        outputs[i].index = i;
        outputs[i].want_float = 1;
    }
    ret = rknn_outputs_get(app_ctx->rknn_ctx, n_output, outputs, NULL);
    if (ret < 0)
    {
        LogError("MeloTTS: rknn_outputs_get failed ret=%d", ret);
        goto out;
    }

    memcpy(logw.data(), (float *)outputs[0].buf, LOGW_SIZE * sizeof(float));
    memcpy(x_mask.data(), (float *)outputs[1].buf, X_MASK_SIZE * sizeof(float));
    memcpy(g.data(), (float *)outputs[2].buf, G_SIZE * sizeof(float));
    memcpy(m_p.data(), (float *)outputs[3].buf, M_P_SIZE * sizeof(float));
    memcpy(logs_p.data(), (float *)outputs[4].buf, LOGS_P_SIZE * sizeof(float));

out:
    // Remeber to release rknn output
    rknn_outputs_release(app_ctx->rknn_ctx, n_output, outputs);
    for (int i = 0; i < n_input; i++)
    {
        if (inputs[i].buf != NULL)
        {
            free(inputs[i].buf);
        }
    }

    return ret;
}

// 将float特征按张量scale/zp量化为int8定点数据，供INT8模型输入使用
static int8_t* quantize_to_int8(const float *src, int n_elems, rknn_tensor_attr *attr)
{
    if (src == nullptr || n_elems <= 0 || attr == nullptr)
    {
        return nullptr;
    }
    int8_t *dst = (int8_t *)malloc(n_elems);
    if (dst == nullptr)
    {
        return nullptr;
    }
    float scale = attr->scale;
    int zp = attr->zp;
    for (int i = 0; i < n_elems; i++)
    {
        // 修改这一行
        float q = roundf(src[i] / scale) + zp;
        if (q > 127.0f) q = 127.0f;
        if (q < -128.0f) q = -128.0f;
        dst[i] = static_cast<int8_t>(q);
    }
    return dst;
}

// 由 attention 与 prior 运行 decoder，输出 float 波形样本。
int inference_decoder_model(melotts_rknn_context_t *app_ctx, std::vector<float> &attn, std::vector<float> &y_mask, std::vector<float> &g, 
    std::vector<float> &m_p, std::vector<float> &logs_p, int &predicted_lengths_max_real, std::vector<float> &output_wav_data)
{
    int ret;
    int n_input = app_ctx->io_num.n_input;
    int n_output = app_ctx->io_num.n_output;
    float noise_scale = NOISE_SCALE;
    int safe_len;
    size_t copy_elem;
    //调试参数
    float *output_float = nullptr;
    float sum = 0.0f;
    float max_val = 0.0f;
    int n = 0;
    size_t total_elem = 0;

    rknn_input inputs[n_input];
    rknn_output outputs[n_output];
    memset(inputs, 0, sizeof(inputs));
    memset(outputs, 0, sizeof(outputs));

    // 封装输入填充逻辑：自动适配FP32 / INT8量化输入
    auto set_input = [&](int idx, const float *src, int elem_num) {
        inputs[idx].index = idx;
        rknn_tensor_attr *attr = &app_ctx->input_attrs[idx];
        if (attr->type == RKNN_TENSOR_INT8)
        {
            // INT8量化分支：CPU浮点转定点int8，pass_through=0 让驱动做拷贝
            // fmt=RKNN_TENSOR_UNDEFINED 避免驱动按NCHW布局错误解析平坦数据
            inputs[idx].type = RKNN_TENSOR_INT8;
            inputs[idx].fmt  = RKNN_TENSOR_UNDEFINED;
            inputs[idx].size = elem_num;
            inputs[idx].buf = quantize_to_int8(src, elem_num, attr);
        }
        else
        {
            // FP32浮点分支，兼容未量化模型
            inputs[idx].type = RKNN_TENSOR_FLOAT32;
            inputs[idx].fmt  = RKNN_TENSOR_UNDEFINED;
            inputs[idx].size = elem_num * sizeof(float);
            inputs[idx].buf = (float *)malloc(inputs[idx].size);
            memcpy(inputs[idx].buf, src, inputs[idx].size);
        }
    };

    /*
    // Set Input Data
    // ["attn", "y_mask", "g", "m_p", "logs_p", "noise_scale"],
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_FLOAT32;
    inputs[0].size = ATTN_SIZE * sizeof(float);
    inputs[0].buf = (float *)malloc(inputs[0].size);
    memcpy(inputs[0].buf, attn.data(), inputs[0].size);

    inputs[1].index = 1;
    inputs[1].type = RKNN_TENSOR_FLOAT32;
    inputs[1].size = Y_MASK_SIZE * sizeof(float);
    inputs[1].buf = (float *)malloc(inputs[1].size);
    memcpy(inputs[1].buf, y_mask.data(), inputs[1].size);

    inputs[2].index = 2;
    inputs[2].type = RKNN_TENSOR_FLOAT32;
    inputs[2].size = G_SIZE * sizeof(float);
    inputs[2].buf = (float *)malloc(inputs[2].size);
    memcpy(inputs[2].buf, g.data(), inputs[2].size);

    inputs[3].index = 3;
    inputs[3].type = RKNN_TENSOR_FLOAT32;
    inputs[3].size = M_P_SIZE * sizeof(float);
    inputs[3].buf = (float *)malloc(inputs[3].size);
    memcpy(inputs[3].buf, m_p.data(), inputs[3].size);

    inputs[4].index = 4;
    inputs[4].type = RKNN_TENSOR_FLOAT32;
    inputs[4].size = LOGS_P_SIZE * sizeof(float);
    inputs[4].buf = (float *)malloc(inputs[4].size);
    memcpy(inputs[4].buf, logs_p.data(), inputs[4].size);

    inputs[5].index = 5;
    inputs[5].type = RKNN_TENSOR_FLOAT32;
    inputs[5].size = 1 * sizeof(float);
    inputs[5].buf = (float *)malloc(inputs[5].size);
    memcpy(inputs[5].buf, &noise_scale, inputs[5].size);
    */

    // ======================================================================
    // 动态模型必须更新本轮推理输入维度，使用当前SDK支持的attr入参版本rknn_set_input_shape接口
    // 从已缓存的input_attrs拷贝张量基础属性，修改动态维度后下发给RKNN上下文
    // 作用：告知NPU本次真实序列长度，避免按最大编译维度解析导致输出数据错乱崩溃，触发vector越界崩溃
    int seq_len = static_cast<int>(attn.size()) / 256;
    // 新增：计算当前推理真实有效元素个数
    int valid_attn_elem = seq_len * 256;
    int valid_y_mask_elem = seq_len;
    rknn_tensor_attr attr;

    // input0 attn [1, seq_len, 256]
    attr = app_ctx->input_attrs[0];
    attr.n_dims = 3;
    attr.dims[0] = 1;
    attr.dims[1] = seq_len;
    attr.dims[2] = 256;
    ret = rknn_set_input_shape(app_ctx->rknn_ctx, &attr);
    if(ret != RKNN_SUCC) {
        LogError("set attn shape failed, ret=%d", ret);
        goto out;
    }

    // input1 y_mask [1, 1, seq_len]
    attr = app_ctx->input_attrs[1];
    attr.n_dims = 3;
    attr.dims[0] = 1;
    attr.dims[1] = 1;
    attr.dims[2] = seq_len;
    ret = rknn_set_input_shape(app_ctx->rknn_ctx, &attr);
    if(ret != RKNN_SUCC) {
        LogError("set y_mask shape failed, ret=%d", ret);
        goto out;
    }

    // input2 g [1,256,1]
    attr = app_ctx->input_attrs[2];
    attr.n_dims = 3;
    attr.dims[0] = 1;
    attr.dims[1] = 256;
    attr.dims[2] = 1;
    ret = rknn_set_input_shape(app_ctx->rknn_ctx, &attr);
    if(ret != RKNN_SUCC) {
        LogError("set g shape failed, ret=%d", ret);
        goto out;
    }

    // input3 m_p [1,192,256]
    attr = app_ctx->input_attrs[3];
    attr.n_dims = 3;
    attr.dims[0] = 1;
    attr.dims[1] = 192;
    attr.dims[2] = 256;
    ret = rknn_set_input_shape(app_ctx->rknn_ctx, &attr);
    if(ret != RKNN_SUCC) {
        LogError("set m_p shape failed, ret=%d", ret);
        goto out;
    }

    // input4 logs_p [1,192,256]
    attr = app_ctx->input_attrs[4];
    attr.n_dims = 3;
    attr.dims[0] = 1;
    attr.dims[1] = 192;
    attr.dims[2] = 256;
    ret = rknn_set_input_shape(app_ctx->rknn_ctx, &attr);
    if(ret != RKNN_SUCC) {
        LogError("set logs_p shape failed, ret=%d", ret);
        goto out;
    }

    // input5 noise_scale [1]
    attr = app_ctx->input_attrs[5];
    attr.n_dims = 1;
    attr.dims[0] = 1;
    ret = rknn_set_input_shape(app_ctx->rknn_ctx, &attr);
    if(ret != RKNN_SUCC) {
        LogError("set noise_scale shape failed, ret=%d", ret);
        goto out;
    }
    // =================================================================

    // 填充6路输入，使用vector真实长度，杜绝越界读取
    // 填充6路输入，使用真实有效长度，去掉多余0
    set_input(0, attn.data(), valid_attn_elem);
    set_input(1, y_mask.data(), valid_y_mask_elem);
    set_input(2, g.data(), static_cast<int>(g.size()));
    set_input(3, m_p.data(), static_cast<int>(m_p.size()));
    set_input(4, logs_p.data(), static_cast<int>(logs_p.size()));
    set_input(5, &noise_scale, 1);

    ret = rknn_inputs_set(app_ctx->rknn_ctx, n_input, inputs);
    if (ret < 0)
    {
        LogError("MeloTTS: rknn_inputs_set failed ret=%d", ret);
        goto out;
    }

    // Run
    // std::cout << "inference_decoder_model rknn_run : " << std::endl;
    ret = rknn_run(app_ctx->rknn_ctx, nullptr);
    if (ret < 0)
    {
        LogError("MeloTTS: rknn_run failed ret=%d", ret);
        goto out;
    }

    // Get Output
    // ['y']
    outputs[0].want_float = 1;
    ret = rknn_outputs_get(app_ctx->rknn_ctx, n_output, outputs, NULL);
    if (ret < 0)
    {
        LogError("MeloTTS: rknn_outputs_get failed ret=%d", ret);
        goto out;
    }

    /*
    // ========== 新增调试打印 ==========
    output_float = (float *)outputs[0].buf;
    sum = 0.0f;
    max_val = 0.0f;
    total_elem = outputs[0].size / sizeof(float);
    n = static_cast<int>(std::min((size_t)100, total_elem));
    //n = std::min(100, outputs[0].size / sizeof(float)); // 只打印前 100 个值
    for (int i = 0; i < n; i++) {
        float v = output_float[i];
        sum += fabs(v);
        if (fabs(v) > max_val) max_val = fabs(v);
    }
    LogInfo("INT8 Decoder output: first %d samples sum_abs=%.6f, max_abs=%.6f", n, sum, max_val);
    // ===================================
    */

    safe_len = std::max(1, predicted_lengths_max_real);
    copy_elem = static_cast<size_t>(safe_len) * PREDICTED_BATCH;
    if (copy_elem > output_wav_data.size())
    {
        output_wav_data.resize(copy_elem);
    }
    memcpy(output_wav_data.data(), (float *)outputs[0].buf, copy_elem * sizeof(float));
    //memcpy(output_wav_data.data(), (float *)outputs[0].buf, predicted_lengths_max_real * PREDICTED_BATCH * sizeof(float));

out:
    // Remeber to release rknn output
    rknn_outputs_release(app_ctx->rknn_ctx, n_output, outputs);
    for (int i = 0; i < n_input; i++)
    {
        if (inputs[i].buf != NULL)
        {
            free(inputs[i].buf);
        }
    }

    return ret;
}

// 单句端到端合成：encoder → middle_process → decoder。
int inference_melotts_model(rknn_melotts_context_t *app_ctx, std::vector<int64_t> &phones,
    int64_t phone_len, std::vector<int64_t> &tones, std::vector<int64_t> &lang_ids,
    int64_t speaker_id, float speed, bool disable_bert, std::vector<float> &output_wav_data)
{
    int ret;
    TIMER timer;
    std::vector<float> logw(LOGW_SIZE);
    std::vector<float> m_p(M_P_SIZE);
    std::vector<float> logs_p(LOGS_P_SIZE);
    std::vector<float> x_mask(X_MASK_SIZE);
    std::vector<float> g(G_SIZE);

    // std::vector<float> bert(1*1024*256, 0.0f);
    std::vector<float> ja_bert;
    if(disable_bert)
    {
        ja_bert.resize(1*768*256, 0.0f);
    }
    else
    {
        //TODO
        return -1;
    }

    // encoder
    timer.tik();
    ret = inference_encoder_model(&app_ctx->encoder_context, phones, phone_len, speaker_id, tones, lang_ids, ja_bert, logw,  x_mask, g, m_p, logs_p);
    if (ret != 0)
    {
        LogError("MeloTTS: inference_encoder_model fail! ret=%d", ret);
        return ret;
    }
    timer.tok();
    LogMeloPhaseTime(timer, "inference_encoder_model");

    // middle
    timer.tik();
    int predicted_lengths_max_real = 0;
    std::vector<float> y_mask(Y_MASK_SIZE, 0.0f);
    std::vector<float> attn(ATTN_SIZE, 0.0f);
    middle_process(logw, x_mask, attn, y_mask, speed, predicted_lengths_max_real);
    timer.tok();
    LogMeloPhaseTime(timer, "middle_process");

    /*
    // ===== 新增保存校准数据（仅在收集数据时启用） =====
    static int calib_counter = 0;
    char fname[256];

    // 保存 attn（整个向量，大小固定 ATTN_SIZE = 512*256）
    snprintf(fname, sizeof(fname), "real_calib/attn_%04d.bin", calib_counter);
    FILE* fp = fopen(fname, "wb");
    fwrite(attn.data(), sizeof(float), attn.size(), fp);
    fclose(fp);

    // 保存 y_mask（整个向量，大小固定 Y_MASK_SIZE = 512）
    snprintf(fname, sizeof(fname), "real_calib/y_mask_%04d.bin", calib_counter);
    fp = fopen(fname, "wb");
    fwrite(y_mask.data(), sizeof(float), y_mask.size(), fp);
    fclose(fp);

    // 保存 g（固定大小 256）
    snprintf(fname, sizeof(fname), "real_calib/g_%04d.bin", calib_counter);
    fp = fopen(fname, "wb");
    fwrite(g.data(), sizeof(float), g.size(), fp);
    fclose(fp);

    // 保存 m_p（固定大小 192*256）
    snprintf(fname, sizeof(fname), "real_calib/m_p_%04d.bin", calib_counter);
    fp = fopen(fname, "wb");
    fwrite(m_p.data(), sizeof(float), m_p.size(), fp);
    fclose(fp);

    // 保存 logs_p（固定大小 192*256）
    snprintf(fname, sizeof(fname), "real_calib/logs_p_%04d.bin", calib_counter);
    fp = fopen(fname, "wb");
    fwrite(logs_p.data(), sizeof(float), logs_p.size(), fp);
    fclose(fp);

    // 保存 noise_scale（固定值 0.6）
    float ns = NOISE_SCALE;
    snprintf(fname, sizeof(fname), "real_calib/noise_scale_%04d.bin", calib_counter);
    fp = fopen(fname, "wb");
    fwrite(&ns, sizeof(float), 1, fp);
    fclose(fp);

    calib_counter++;
    // ===== 保存结束 ============================
    */

    // decoder
    timer.tik();
    ret = inference_decoder_model(&app_ctx->decoder_context, attn, y_mask, g, m_p, logs_p, predicted_lengths_max_real, output_wav_data);
    if (ret != 0)
    {
        LogError("MeloTTS: inference_decoder_model fail! ret=%d", ret);
        return ret;
    }
    timer.tok();
    LogMeloPhaseTime(timer, "inference_decoder_model");

    return predicted_lengths_max_real;
}

