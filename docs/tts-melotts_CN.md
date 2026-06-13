Language: **中文** | [English](tts-melotts.md)

# TTS 与语音对话体验设计

> **唯一 TTS 设计与验收文档**。实现代码：`runtime/adapters/tts/`；配置：`runtime/config/default.yaml` → `model.tts`（启动时仍要求 `model.llm.enabled: true`）。

---

## 1. 目标体验

面向当前板端「有人脸对话」参考应用，语音交互体感应为：

```text
人脸稳定 → 静态问候（文字+语音，PlayText）
→ 用户 YOU> → LLM 流式 AI> → TtsPlanner 规划 → Melo 合成 → 连续播报正式回答
```

用户感知：

- 问候语（如「您好，需要帮忙吗」）**句首清楚、整句连贯**；
- 正式回答 **不中途吃字、不明显断档**；**句首文字可听见**；
- 新 `YOU>` 可打断旧回答（`generation_` 抢占）；
- 人脸 Grace/Locked 后拒绝新输入。

**设计原则**：优先 **听感连续、句首可辨、整句完整**；不追求「尽快切块播」。Melo decoder 单次约 **1.6–2.2s** 且多句只能串行，需在 Planner / 合成 / 播放三层配合。

不播报：`SYS>`、用户输入行、视觉 debug、`<think>` 块；合成前由 `TtsTextSanitizer` 去掉 emoji 等不发音字符。

技术选型：板端 **MeloTTS `ZH_MIX_EN`**（encoder/decoder RKNN + `lexicon.txt` / `tokens.txt`）。TTS 与 RKLLM 为旁路，**不进**视觉 Pipeline。

板端需 **`gst-launch-1.0`**（GStreamer 管道播放）。

---

## 2. 验收标准

### 必须通过

| 项 | 标准 |
|----|------|
| 静态问候 | 人脸稳定后问候 **文字+语音**；句首清楚（与正式回答对照基准） |
| 正式回答连续性 | 短答整段连贯；长答 job 内 PCM 合并后一次播放，**无块间吃字** |
| 句首可辨 | 正式回答开头 3–4 字/词 **可听见**（非仅屏幕有字） |
| 英文 | 禁止 Planner **单词级**切分；短英文整段 single-shot |
| 门控 | Grace 超时后拒绝新输入 |
| 视觉 | 无 `mode:none`；TTS 活跃时 **仅跳 yolo inference**，scrfd 保持 |
| 抢占 | 新 `YOU>` → `Cancel()` + `generation_++`，仅播最新代际 |
| 配置 | `enabled=false` 无 TTS；`skip_static_greeting=true` 不播问候 |

### 可接受取舍

| 项 | 说明 |
|----|------|
| 正式回答首响 | LLM FINISH + Melo 串行时间（短答 ~1 decoder ≈2s；长答多 decoder 更晚） |
| 长答延迟 | 宁可等 job 内合成合并完再播，也不边产边播造成 underrun |
| 屏显 vs 耳上 | 终端 `AI>` 流式早于 TTS 首响属正常；**验收以耳朵为准** |

### 不作为单独 Pass 的指标

| 项 | 说明 |
|----|------|
| `pcm underrun` 日志 | 仅在下一段仍在合成且队列见底时计数；**underrun=0 不等于听感 Pass** |
| `first pcm enqueued` 与 `played` 同毫秒 | 表示已写入 gst，不保证句首振幅足够 |

---

## 3. 模块边界

| 模块 | 职责 |
|------|------|
| `tts_ingress.*` | 过滤 thinking/tag；UTF-8 整理；LLM chunk event 入口 |
| `tts_planner.*` | 中/英正式回答规划；短答 FINISH 整段；长答流式 emit |
| `tts_worker.*` | Static / FormalAnswer 文本队列；合成+播放双线程；`generation_` 抢占 |
| `melotts_session.*` | RKNN；`split_sentence` 或 **single-shot**；`SynthesizeTextStreaming` |
| `audio_player.*` | 单实例 `gst-launch-1.0`；空闲 priming；`Cancel` 不杀管道 |
| `tts_text_sanitizer.*` | 去 emoji；`max_speak_chars` 截断 |
| `lexicon.hpp` / `split.hpp` | 词表；Melo 内部分句 |

平台关系：

- **LLM**：`OnLlmChunk` 投递事件 → 主线程 `PollDeferred` → Ingress/Planner → `EnqueueFormalAnswer`。
- **问候**：`LlmGreeting::SetBannerLine` → **`PlayText`（Static）**，不经 Planner，与短答 Formal 路径对齐。
- **抢占**：`SubmitPrompt` → **`TtsWorker::Cancel()`**（清队列 + `generation_++`，**不**杀 gst）。
- **视觉**：TTS 活跃期可跳 yolo inference（`ShouldSkipYoloForDialogueTts()`）。

---

## 4. 数据流

```text
【问候】人脸稳定
  → SetBannerLine(问候, is_final)
  → PlayText → TextJobKind::Static
  → Melo（single-shot 或分句）→ PushPcmChunk(Static) → PlaybackLoop → gst

【正式回答】YOU> accepted
  → SubmitPrompt → TtsWorker::Cancel()
  → rkllm_run → OnLlmChunk
  → TtsIngress → TtsPlanner
       · 累计 ≤ short_answer_max_chars：Feed 不 emit，FINISH 整段 Flush
       · 更长：按 zh/en 阈值 + fallback_timeout_ms 流式 emit
  → EnqueueFormalAnswer
       · ≤ single_shot_max_chars：入队为 Static（与 PlayText 同链）
       · 更长：FormalAnswer
  → SynthesizeLoop
       · Static / 短 Formal：逐 Melo 句 → trim → PushPcm（短答 Melo 走 single-shot）
       · 长 Formal：job 内多句 PCM **合并一块** → 一次 PushPcm
  → PlaybackLoop（pcm_queue 非空即播）→ AudioPlayer（空闲 priming）→ gst
```

---

## 5. Static 问候 vs Formal 正式回答

| | 静态问候 `PlayText` | 正式回答 `EnqueueFormalAnswer` |
|--|---------------------|--------------------------------|
| 触发 | 人脸稳定，`SetBannerLine` 同时入 TTS | LLM FINISH/流式 Planner 段 |
| 入队类型 | 始终 `TextJobKind::Static` | 短答 **Static**；长答 FormalAnswer |
| 合成 | `SynthesizeTextStreaming`；仅 `TrimAbsoluteLeadingSilence` | 短答同 Static；长答 job 内 merge |
| PCM 种类 | `PcmJobKind::Static` | Static 或 `PcmJobKind::Formal` |
| 前置 | 无 `Cancel` | **`SubmitPrompt` 先 `Cancel()`** |

**板端排障结论**（2025 对话迭代）：

- 问候正常、正式句首听不见时，**首要怀疑 Formal 与 Static 路径差异**，而非「Melo 模型天生句首弱」。
- `Cancel()` 后 LLM 推理期间 gst **长时间无写入**，下次 `PlayPcm` 可能 **吞掉 PCM 头** → `audio_player` 对空闲 ≥300ms 做 **priming**（可丢弃静音）。
- 对合并 PCM 做 **RMS/峰值句首裁剪** 曾误删整句（弱起音第一句 + 强起音第二句）→ **禁止**对 merge 后整段做 RMS trim。

---

## 6. TtsIngress / TtsTextSanitizer

- Ingress：过滤 `<think>`；UTF-8 边界；**不做**时间硬切。
- Sanitizer：去掉 **UTF-8 四字节 emoji** 等不发音字符；`max_speak_chars` 截断。

---

## 7. TtsPlanner

流式 `Feed` → `TryEmitSegments`；`FINISH` → `Flush` 尾段。

| 语言 | 策略 |
|------|------|
| 中文 | 句界优先；`zh_max_chars` 或 `zh_min_chars` + `fallback_timeout_ms` |
| 英文 | **禁止**单词级 emit；`en_max_words` 或 `en_min_words` + fallback |

**短答 defer**（`short_answer_max_chars`，默认 96）：

- `pending_` 累计字数 ≤ 阈值时 **`Feed` 不 emit**，等 **FINISH 一次性**下发整段。
- 避免 800ms 硬切把一句短答拆成多 job、多次 decoder、边播边产 underrun。

长答仍流式 emit；`TtsWorker::CoalesceFormalAnswerLocked` 在合成前再合并相邻 Formal 块。

---

## 8. Melo 合成策略

| 条件 | 行为 |
|------|------|
| `utf8_strlen(text) ≤ single_shot_max_chars`（默认 96） | **整段一次** `SynthesizeOneSentenceUnlocked`，不调用 `split_sentence` |
| 更长 | `split_sentence(text, split_min_chars, language)` 多句串行 decoder |

硬约束：

- 单次 decoder ~1.6–2.2s；多句 **只能串行**（单合成线程 + session 锁）。
- 超过 `PREDICTED_LENGTHS_MAX` 会 **截断句尾**（log：`predicted_lengths_max_real > PREDICTED_LENGTHS_MAX`）→ 禁止对超长文本 single-shot，靠 Planner/Melo 分句。

---

## 9. TtsWorker 播放策略

**双线程**：`SynthesizeLoop` + `PlaybackLoop`。

| 场景 | 合成 | 播放 |
|------|------|------|
| Static / 短 Formal（≤96 字） | single-shot 或分句；句首 **仅裁绝对静音**（`<5e-5`） | `pcm_queue` 非空即播 |
| 长 Formal | job 内 `AppendMeloSentencePcm` 合并 → 一次 PushPcm | 同上 |

**禁止**（曾导致回退的失败改法）：

- Formal 边产边播多块 PCM（块间 underrun / 吃字）；
- 合并后整段 RMS/峰值 `TrimLeadingSilence`（误删弱起音整句）；
- 无上限 aggressive trim / 整篇长答 single-shot 超模型上限。

**PCM 句首处理**：`TrimAbsoluteLeadingSilence` 只删近零样本，保留 10ms pad；**不**做弱起音增益（易与 merge trim 叠加出问题）。

**代际统计**：`first pcm enqueued/played`、`underrun`（仅 pipeline pending 且队列空时）。

---

## 10. AudioPlayer（gst）

- 单实例 `gst-launch-1.0`：`fdsrc ! queue ! audioconvert ! audioresample ! autoaudiosink`。
- `Cancel()` **不**终止子进程，保持管道可连续写。
- **Cold prime**：管道新建后先写 ~50ms 静音。
- **Idle prime**：距上次 PCM 写入 ≥ **300ms**（典型：`Cancel` + LLM 推理空档）先写 ~100ms 可丢弃静音，再写真实 PCM。log：`AudioPlayer: primed stream (...)`。

---

## 11. 与视觉 / 门控

TTS 活跃 + 人脸对话 Active/Grace → 跳过 **yolo inference only**；scrfd 与 Grace/Locked **不变**。禁止 TTS QoS 导致 `mode:none` 或关 scrfd。

---

## 12. 配置

见 [`default.yaml`](../runtime/config/default.yaml) → `model.tts`。

| 字段 | 说明 |
|------|------|
| `enabled` | TTS 总开关 |
| `skip_static_greeting` | `true` 不播静态问候 |
| `split_min_chars` | Melo **长文本**内部分句阈值 |
| `single_shot_max_chars` | 不超过则 **整段一次 decoder**；应 ≥ `planner.short_answer_max_chars` |
| `planner.short_answer_max_chars` | 短答 FINISH 整段，流式不中途 emit |
| `planner.zh_min/max_chars`、`en_min/max_words` | 长答流式 emit |
| `planner.fallback_timeout_ms` | 未达 max 时的超时 emit（长答） |
| `qos.min_start_pcm_chunks` | 现默认 **1**（job 合并后多为 1 块） |
| `qos.enable_visual_throttle` | 低水位跳 yolo infer |

---

## 13. 日志与排障

| 日志 | 含义 |
|------|------|
| `first pcm enqueued/played in X ms` | 首条文本 → 首个 PCM（含 Melo 串行时间） |
| `pcm underrun #N` | 播完队列空且仍在合成；**≠ 听感断粮唯一依据** |
| `AudioPlayer: primed stream` | 空闲后 priming，保护句首 |
| 多次 `inference_decoder_model` | 文本超 single-shot 或长答多句；短答应 **1 次** |
| `predicted_lengths_max_real > PREDICTED_LENGTHS_MAX` | 单次文本过长，**句尾被截** |

### 句首听不见 / 只吃后半句

1. **对照问候**：若 `PlayText` 正常、Formal 不正常 → 查 Static vs Formal 路径、`Cancel` 后 idle priming（是否有 `primed stream` log）。
2. **decoder 次数**：短答应 1 次；若 2+ 次 → 检查 `single_shot_max_chars` / `short_answer_max_chars` 是否生效、是否重新编译部署。
3. **禁止** merge 后 RMS 裁剪；仅绝对静音 trim。
4. **underrun + 吃字**：是否边产边播；长/job 内应 **merge 再播**。
5. **OOV / 英文**：lexicon 字母回退。

### 曾出现的问题与修复摘要

| 现象 | 根因 | 修复 |
|------|------|------|
| 句间吃字、underrun | Formal 边产边播多块 PCM | job 内 merge，一次 PlayPcm |
| 短答句首无声音 + 中间吃字 | Planner 800ms 切碎 + 边播 | `short_answer_max_chars` defer；短答 Static 入队 |
| 句首文字听不见 | Formal 路径与 Static 不一致；`Cancel` 后 gst 空闲 | 短答 Static 入队；idle priming |
| 整句被裁没 | merge 后 RMS 找起音裁到第二句 | 已回退；仅 per-chunk 绝对静音 trim |

平台摘要：[architecture-and-runtime_CN.md](architecture-and-runtime_CN.md) §8。

---

*若与代码不一致，以 `runtime/` 为准；验收以本文 §2 为准。*
