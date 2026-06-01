Language: **中文** | [English](adapters.md)

# 适配器说明

视觉与逻辑适配器速览；细节见各专题文档。

---

## YOLO（`adapters/yolo/`）

- 正点原子 YOLOv5 三头 RKNN（`output num: 3`，`dims[1]=255`）。
- 后处理：`yolo_postprocess.cpp` 固定解码 `output[0..2]`。
- 排障见 [troubleshooting_CN.md](troubleshooting_CN.md) § 视觉模型排障。

---

## SCRFD（`adapters/scrfd/`）

- 9 路输出（`score_*` / `bbox_*` / `kps_*`），分组布局。
- 后处理：`scrfd_postprocess.cpp` 中 `ResolveScrfdHeadOutputs`。
- 排障见 [troubleshooting_CN.md](troubleshooting_CN.md) § 视觉模型排障。

---

## LLM（`adapters/llm/`）

与 `adapters/yolo`、`adapters/scrfd` 同级，但 **不** 实现 `IModelAdapter` 每帧推理。

| 文件 | 职责 |
|------|------|
| `rkllm_session.*` | `rkllm_init` / **`rkllm_run`（同步）** / `rkllm_abort` / `rkllm_destroy`；回调 NORMAL 直写 stdout |
| `llm_worker.*` | 异步加载；`infer_thread_` 跑 `RunPromptSync`；`OnLlmChunk` 投递 TTS chunk event；主线程 `PollDeferred` 处理 |

配置：`model.llm.*`。集成见 [llm-model-coordinator_CN.md](llm-model-coordinator_CN.md)。

**两条输出路径：**

- **用户对话**：`SubmitPrompt` → `rkllm_run` → 流式 `AI>`（含 thinking 显示）→ chunk event → TTS。
- **自动问候**：`LlmGreeting` → `SetBannerLine`（静态 yaml）；不经 RKLLM。

---

## TTS / MeloTTS（`adapters/tts/`）

### 作用

将 **`AI>` 可播正文** 合成为语音（44100Hz）：

- 静态问候（`SetBannerLine` → `PlayText`）
- `YOU>` 后正式回答（FastAck 短反馈 + TtsPlanner 流式规划 → `EnqueueFormalAnswer`）

终端可见 thinking；**不进入 TTS**。

### 模块

| 文件 | 职责 |
|------|------|
| `tts_ingress.*` | thinking/tag 过滤；UTF-8 输入整理 |
| `tts_planner.*` | 流式规划正式回答片段；中/英 emit 阈值 |
| `tts_worker.*` | FastAck / FormalAnswer；合成前合并批次；`generation_` 抢占 |
| `melotts_session.*` | RKNN 合成；`SynthesizeTextStreaming` 句级增量 PCM |
| `audio_player.*` | 常驻 `gst-launch-1.0` 管道写 float32 PCM |
| `tts_text_sanitizer.*` | 仅 `max_speak_chars` 截断 |
| `lexicon.hpp` / `split.hpp` | 词表；英文 OOV 字母回退 |

### 模型与配置

板端 `./model/`：`encoder-ZH_MIX_EN.rknn`、`decoder-ZH_MIX_EN.rknn`、`lexicon.txt`、`tokens.txt`。

YAML：`model.tts.*`（与 `model.llm` 并列；启动仍要求 `model.llm.enabled: true`）。

### 播放

`AudioPlayer` 使用常驻 **`gst-launch-1.0`** 管道，向 stdin 写入 float32 PCM（**非** 每段 `gst-play` 播 wav 文件）。

设计与验收见 [tts-melotts_CN.md](tts-melotts_CN.md)。
