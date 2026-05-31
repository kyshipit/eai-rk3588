# TTS 与语音对话体验设计

> **唯一 TTS 设计与验收文档**。实现代码：`runtime/adapters/tts/`；配置：`runtime/config/default.yaml` → `model.tts`（启动时仍要求 `model.llm.enabled: true`）。

---

## 1. 目标体验

面向当前板端「有人脸对话」参考应用，语音交互体感应为：

```text
人脸稳定 → 用户输入 accepted → ≤1s 听到短反馈（如「好的」「我看看」）
→ LLM 生成正式回答 → TtsPlanner 规划片段
→ FormalAnswerBuffer 达到安全缓冲 → 连续播正式回答
```

用户感知：

- 机器人**马上**有声音回应，不是长时间静音等待；
- 正式回答稍后接上，**中途不一词一卡**、不出现明显断档；
- 用户再次输入可打断旧回答；
- 人脸离开后按 Grace/Locked 规则拒绝新输入。

**设计原则**：语音对话体验目标不是「尽快切块播」，而是 **≤1s 短反馈 + 正式回答开播后 `pcm underrun=0`**。

不播报：`SYS>`、用户输入、视觉检测日志、`<think>` 块。

技术选型：板端 **MeloTTS `ZH_MIX_EN`**（encoder/decoder RKNN + `lexicon.txt` / `tokens.txt`）。TTS 与 RKLLM 同为旁路，**不进**视觉 Pipeline / `IModelCoordinator`。

---

## 2. 最终验收标准

### 必须通过

| 项 | 标准 |
|----|------|
| 短反馈首响 | `YOU>` accepted 后 **≤1s** 有缓存短反馈音；目标 300–800ms |
| 正式回答连续性 | 正式回答一旦开播，**`pcm underrun=0`** |
| 英文连续性 | 不允许英文单词级合成 / 单词级卡顿 |
| 门控正确性 | 人脸离开 Grace 超时后拒绝新输入 |
| 视觉状态 | 不出现 `mode:none`；TTS 活跃时只跳 yolo inference，**不关 scrfd** |
| 抢占 | 新 `YOU>` 后旧 generation/session 丢弃，仅播最新 |
| 配置关闭 | `model.tts.enabled=false` 无 TTS；`skip_static_greeting=true` 不播静态问候 |
| 问候 | 默认配置：人脸稳定后问候 **文字与语音**（品牌英文依赖词表/字母回退） |

### 可接受取舍

| 项 | 说明 |
|----|------|
| 正式回答首句不保证 ≤1s | ≤1s 由 **FastAck** 保证；正式回答可后台合成后接上 |
| 正式回答可能稍晚 | 宁可晚播，也不能开播后卡断 |
| TTS 活跃时视觉可降载 | 只跳 yolo inference，scrfd / 门控保持 |

### 不再作为验收的旧标准

- 「不等待全文 FINISH 就开始分块播报」不是充分标准；
- 「短答 ≤2s、长答 ≤4s」不能代表机器人首响体验；
- `emit_timeout_ms` / `TtsStreamBuffer` 时间硬切 **不再作为正式设计**。

板端需已安装 **`gst-launch-1.0`**（GStreamer 管道播放）。

---

## 3. 模块边界

| 模块 | 职责 |
|------|------|
| `tts_ingress.*` | 过滤 thinking/tag；UTF-8 输入整理；LLM chunk event 入口 |
| `tts_planner.*` | 中文/英文正式回答规划：合并短语、限长、避免单词级切分 |
| `tts_worker.*` | FastAck / FormalAnswer 队列；合成/播放双线程；`generation_` 抢占 |
| `melotts_session.*` | RKNN 合成；分句；`SynthesizeTextStreaming` 句级增量 PCM |
| `audio_player.*` | 单实例 `gst-launch-1.0` 管道持续写 float32 PCM |
| `lexicon.hpp` / `split.hpp` | 词表；英文 OOV 按字母回退 |
| `tts_text_sanitizer.*` | 仅 `max_speak_chars` 截断 |

> **实现状态**：目标架构为 FastAck + TtsIngress + TtsPlanner + FormalAnswerBuffer。过渡期代码中 `tts_stream_buffer.*` 仍承担 ingress/规划职责，待迁移后删除。

与平台关系：

- **LLM**：`LlmWorker::OnLlmChunk` 投递 chunk event，主线程 `PollDeferred` 统一处理 TTS。
- **问候**：`LlmGreeting::SetBannerLine` → `TtsWorker::PlayText`（整段，不经 Planner）。
- **视觉**：TTS 活跃期 `Pipeline` 可跳过 yolo inference（`ShouldSkipYoloForDialogueTts()`），**不关闭 yolo/scrfd 槽位**，不破坏 Grace/Locked 门控。
- **抢占**：`SubmitPrompt` / `Abort` / 新 `YOU>` → `TtsWorker::Cancel`（清队列 + `generation_++`，不杀 gst 管道）。

---

## 4. 数据流

```text
人脸稳定 → LlmGreeting::SetBannerLine(问候) → TtsWorker::PlayText（整段）

YOU> accepted
  → FastAck：播放预缓存短反馈 PCM（≤1s）
  → rkllm_run → OnLlmChunk（chunk event）
  → TtsIngress：跳过 <think>
  → TtsPlanner：规划正式回答片段（中/英策略分离）
  → FormalAnswerBuffer：达到 min_start 缓冲后开播
  → TtsWorker::SynthesizeLoop → MeloTtsSession::SynthesizeTextStreaming
  → pcm_queue → PlaybackLoop → gst-launch PCM
```

说明：

- 终端流式显示 **含 thinking**；TTS **仅播** thinking 之外的可见正文（屏显 ≠ 耳上）。
- `reply_accumulator_` 仍供调试/扩展，**不是** TTS 主路径。
- 新 `YOU>` / `Abort` / `Stop` → `TtsWorker::Cancel`。

---

## 5. FastAck 短反馈

**目的**：在 Melo decoder 单次 ~1.6–2.2s 的硬约束下，保证用户输入后 ≤1s 有声音。

- 预生成/预缓存短反馈 PCM（如「好的」「我看看」），`YOU>` accepted 后立即播放。
- FastAck 与正式回答 **解耦**：短反馈不等待 LLM 首 token，正式回答在后台合成。
- 正式回答开播前由 FormalAnswerBuffer 攒够安全缓冲，避免「为防断粮而延迟首声」与「为快首声而切太碎」互相打架。

---

## 6. TtsIngress

- 接收 LLM chunk event（非 RKLLM 回调线程直接合成）。
- 过滤 `<think>…</think>`。
- 保证 UTF-8 边界正确，避免半字符切分。
- **不做**时间硬切：ingress 只整理输入，规划交给 TtsPlanner。

---

## 7. TtsPlanner

**目的**：替代 `TtsStreamBuffer` 的时间硬切，按语言语义规划正式回答片段。

| 语言 | 策略 |
|------|------|
| 中文 | 按句界/短语合并，避免过短块 |
| 英文 | **禁止** token/word 级切分；合并到短语或子句（最小字符/音节阈值） |

原则：

- 单次送入 Melo 的片段应足够长，使 PCM 播放时长 ≥ 单次 decoder 开销；
- 英文 LLM 输出为 word 级 token 时，Planner 须 coalesce 后再合成；
- `emit_timeout_ms` 仅作 fallback，**不是**主体验参数。

---

## 8. FormalAnswerBuffer

- 正式回答开播前缓冲 N 段 PCM（`min_start_pcm_chunks`），达到安全水位再开始播放。
- 播放期间监控 `pcm_queue` 水位；低水位时触发视觉降载（跳 yolo inference）。
- 整轮 TTS 活跃期保持保护，不在 `pcm_queue>=3` 时过早退出。
- **验收指标**：正式回答一旦开播，`underrun=0`。

---

## 9. TtsWorker / MeloTTS / AudioPlayer

- **双线程**：`SynthesizeLoop`（RKNN 合成）+ `PlaybackLoop`（PCM 消费）。
- **MeloTTS**：`SynthesizeTextStreaming` 按句回调 PCM；非音素级真流式。
- **AudioPlayer**：常驻 `gst-launch-1.0` 管道写 stdin；`Cancel` 不杀管道。
- **统计日志**（代际级）：首条文本→首个 PCM 入队/播放耗时、`underrun` 计数。

已知限制：

- Melo decoder 单次 ~1.6–2.2s 是在线合成的硬约束；靠 FastAck + Planner + Buffer 绕开，而非假设模型变快。
- **Lexicon OOV**：整词查表失败可能静音；英文未知词应字母回退，避免整词吞字。
- NPU 与 YOLO/SCRFD/RKLLM 争用；人脸对话期跳 yolo inference 缓解。

---

## 10. 与视觉 / LLM / 门控关系

```text
人脸对话 Active/Grace + TTS 活跃
  → ModelCoordinator::ShouldSkipYoloForDialogueTts() == true
  → Pipeline::RunEnabledSlots 跳过 yolo 推理（不关槽）
  → scrfd 保持 → 门控 / Grace / Locked 逻辑不变
```

**禁止**：

- TTS QoS 导致 `mode:none`；
- 关闭 scrfd 或伪造 person 状态；
- idle 场景因 TTS 关 yolo（无人时不应影响视觉）。

---

## 11. 配置

字段见 [`config/default.yaml`](../runtime/config/default.yaml) → `model.tts`。

| 字段 | 说明 |
|------|------|
| `enabled` | TTS 总开关（需 `model.llm.enabled`） |
| `skip_static_greeting` | `true` 不播静态问候 |
| `encoder_path` / `decoder_path` | RKNN |
| `lexicon_path` / `tokens_path` | 词表 |
| `language` | 如 `ZH_MIX_EN` |
| `speak_id` / `speed` / `disable_bert` | 推理参数 |
| `preload_on_startup` | 启动异步加载 TTS（建议 `true`） |
| `max_speak_chars` | `0` 不截断 |
| `split_min_chars` | Melo 内部分句最小字符 |
| `qos.enable_visual_throttle` | TTS 低水位时跳 yolo inference |
| `qos.low_watermark_chunks` / `high_watermark_chunks` | PCM 队列水位阈值 |
| `qos.min_start_pcm_chunks` | 正式回答开播前缓冲段数 |

---

## 12. 日志与排障

| 日志 | 含义 |
|------|------|
| `first pcm enqueued in X ms` | 首条文本到首个 PCM 入队 |
| `first pcm played in X ms` | 首条文本到首个 PCM 播放 |
| `pcm underrun #N` | 播放断粮（正式回答验收应为 0） |
| `ModelCoordinator: tts yolo-skip on` | 人脸对话期跳过 yolo 推理 |
| `mode:none` | **异常**，查 TTS QoS 是否误关槽 |

排障顺序：

1. 短反馈是否在 ≤1s 内播放（FastAck 缓存是否就绪）；
2. 正式回答 `underrun` 计数；
3. 英文是否 word 级切分（查 Planner / 过渡期 `TtsStreamBuffer` 阈值）；
4. NPU 争用（yolo-skip 是否生效）；
5. OOV 吞词（查 lexicon）。

平台级 TTS 摘要见 [系统架构与运行逻辑.md](系统架构与运行逻辑.md) §9。

---

## 13. 历史问题与设计取舍

### 旁路位置是对的

TTS 不进视觉 Pipeline、与 RKLLM 并列旁路，避免拖慢 YOLO/SCRFD 帧率。播放层常驻 `gst-launch` 管道、合成/播放双线程也是合理选择。

### 真正的问题：目标与范式错配

早期实现是「文本流式 + 块级 RKNN 合成 + 时间硬切」的伪流式：

- `TtsStreamBuffer` 的 `emit_timeout_ms` 时间硬切，英文 token 级输出被切成单词/短语，每块触发 ~2s decoder → **一词一卡**；
- `min_start_pcm_chunks=2` 在无 FastAck 时导致首声等 4s+；
- QoS 低水位保护若通过 `DisableSlot("yolo")` 会破坏参考应用状态机（`mode:none`）。

**结论**：播放层不是主矛盾；主矛盾是 **Melo decoder 慢 + 文本切太碎 + 缺少 FastAck 层**。

### 为什么需要 FastAck + Planner + Buffer

| 组件 | 解决什么 |
|------|----------|
| FastAck | ≤1s 短反馈，掩盖 decoder 首包延迟 |
| TtsPlanner | 语言感知合并，避免英文 word 级合成 |
| FormalAnswerBuffer | 开播前攒缓冲，开播后 `underrun=0` |
| 跳 yolo inference | NPU 让路，不破坏门控 |

### 仍开放（非本验收范围）

- 音素/token 级真流式 PCM；
- VAD/ASR/AEC 与语音 barge-in；
- 更快 TTS 模型或预合成话术库扩展；
- `model.tts` 配置从 `model.llm` 解耦（当前启动仍要求 `llm.enabled`）。

---

*若与代码不一致，以 `runtime/` 代码为准；验收以本文 §2 为准。*
