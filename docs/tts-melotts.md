Language: **English** | [中文](tts-melotts_CN.md)

# TTS and voice dialogue design

> **Sole TTS design and acceptance document**. Code: `runtime/adapters/tts/`; config: `runtime/config/default.yaml` → `model.tts` (startup still requires `model.llm.enabled: true`).

---

## 1. Target experience

For the on-board face-gated reference app, voice should feel like:

```text
Stable face → user input accepted → ≤1s short ack (e.g. “好的”, “我看看”)
→ LLM formal answer → TtsPlanner segments
→ FormalAnswerBuffer reaches safe level → continuous formal playback
```

User perception:

- **Immediate** audio after input, not long silence;
- Formal answer follows without **word-by-word** gaps or obvious underruns;
- New input preempts the previous answer;
- After face leave, new input rejected per Grace/Locked rules.

**Principle**: optimize for **≤1s short ack + `pcm underrun=0` after formal playback starts**, not “play chunks as fast as possible”.

Do not speak: `SYS>`, user input lines, vision debug logs, `<think>` blocks.

Stack: on-board **MeloTTS `ZH_MIX_EN`** (encoder/decoder RKNN + `lexicon.txt` / `tokens.txt`). TTS and RKLLM are side paths, **not** in the vision Pipeline / `IModelCoordinator`.

---

## 2. Acceptance criteria

### Must pass

| Item | Criterion |
|------|-----------|
| Short ack latency | Within **≤1s** after `YOU>` accepted; target 300–800 ms |
| Formal continuity | Once formal playback starts, **`pcm underrun=0`** |
| English continuity | No per-word synthesis / per-word stutter |
| Gate | Reject new input after Grace timeout when face left |
| Vision | No `mode:none`; when TTS active skip **yolo inference only**, **keep scrfd** |
| Preemption | New `YOU>` drops old generation/session; play latest only |
| Config off | `model.tts.enabled=false` → no TTS; `skip_static_greeting=true` → no static greet |
| Greeting | Default: text + voice after stable face (English brands via lexicon/letter fallback) |

### Acceptable trade-offs

| Item | Note |
|------|------|
| Formal first sentence not ≤1s | **FastAck** covers ≤1s; formal may start later |
| Formal may be late | Prefer late start over underrun after start |
| Vision load during TTS | Skip yolo infer only; scrfd and gate unchanged |

Board needs **`gst-launch-1.0`** (GStreamer pipe playback).

---

## 3. Module boundaries

| Module | Role |
|--------|------|
| `tts_ingress.*` | Filter thinking/tags; UTF-8 normalize; LLM chunk event entry |
| `tts_planner.*` | Plan formal segments zh/en; merge phrases; avoid word-level cuts |
| `tts_worker.*` | FastAck / FormalAnswer queues; synth+play threads; `generation_` preemption |
| `melotts_session.*` | RKNN synth; sentence split; `SynthesizeTextStreaming` PCM chunks |
| `audio_player.*` | Single `gst-launch-1.0` pipe, continuous float32 PCM |
| `lexicon.hpp` / `split.hpp` | Lexicon; English OOV letter fallback |
| `tts_text_sanitizer.*` | `max_speak_chars` truncation only |

Platform ties:

- **LLM**: `LlmWorker::OnLlmChunk` posts events; main `PollDeferred` drives TTS.
- **Greeting**: `LlmGreeting::SetBannerLine` → `TtsWorker::PlayText` (whole utterance, no Planner).
- **Vision**: While TTS active, `Pipeline` may skip yolo inference (`ShouldSkipYoloForDialogueTts()`); **does not** disable yolo/scrfd slots or break Grace/Locked.
- **Preemption**: `SubmitPrompt` / `Abort` / new `YOU>` → `TtsWorker::Cancel` (clear queue + `generation_++`, keep gst pipe).

---

## 4. Data flow

```text
Stable face → LlmGreeting::SetBannerLine(greet) → TtsWorker::PlayText (whole)

YOU> accepted
  → FastAck: play cached short PCM (≤1s)
  → rkllm_run → OnLlmChunk (chunk events)
  → TtsIngress: skip <think>
  → TtsPlanner: plan formal segments (zh/en policies)
  → FormalAnswerBuffer: play after min_start buffer
  → TtsWorker::SynthesizeLoop → MeloTtsSession::SynthesizeTextStreaming
  → pcm_queue → PlaybackLoop → gst-launch PCM
```

Notes:

- Terminal stream **includes thinking**; TTS **only** speaks visible text outside thinking (screen ≠ ear).
- New `YOU>` / `Abort` / `Stop` → `TtsWorker::Cancel`.

---

## 5. FastAck

**Goal**: Under Melo decoder ~1.6–2.2 s per call, still hear audio within ≤1 s of accepted input.

- Prebuilt short PCM cache; play immediately on accepted `YOU>`.
- **Decoupled** from formal answer: FastAck does not wait for first LLM token.
- Formal start waits on FormalAnswerBuffer to avoid fighting “delay for buffer” vs “chunk too small”.

---

## 6. TtsIngress

- Consumes LLM chunk events (not direct synth from RKLLM callback thread).
- Strips `<think>…</think>`.
- Valid UTF-8 boundaries.
- **No** hard time slicing; planning is TtsPlanner’s job.

---

## 7. TtsPlanner

Plans formal segments on streaming `Feed` via `TryEmitSegments`; `Flush` on `FINISH`.

| Language | Policy |
|----------|--------|
| Chinese | Sentence boundaries first; emit at `zh_max_chars` or `zh_min_chars` + `fallback_timeout_ms` |
| English | **No** per-word synth; emit at `en_max_words` or `en_min_words` + fallback |

Principles:

- Each Melo call should be long enough (`TtsWorker::CoalesceFormalAnswerLocked`) so playback ≥ one decoder call (~1.6–2.2 s);
- Word-level LLM tokens: merge in Planner + synth before RKNN.

---

## 8. FormalAnswerBuffer

- Buffer N PCM chunks (`min_start_pcm_chunks`) before formal playback starts.
- Monitor `pcm_queue` during play; low watermark triggers yolo infer skip.
- Stay in protection for whole TTS-active period; do not exit early just because `pcm_queue>=3`.
- **Acceptance**: `underrun=0` after formal playback starts.

---

## 9. TtsWorker / MeloTTS / AudioPlayer

- **Two threads**: `SynthesizeLoop` (RKNN) + `PlaybackLoop` (PCM consumer).
- **MeloTTS**: `SynthesizeTextStreaming` sentence callbacks; not true phoneme streaming.
- **AudioPlayer**: persistent `gst-launch-1.0` stdin; `Cancel` does not kill pipe.
- **Stats** (per generation): time to first PCM enqueue/play, `underrun` count.

Known limits:

- Decoder ~1.6–2.2 s per call is a hard constraint; mitigated by FastAck + Planner + Buffer, not faster model assumption.
- **Lexicon OOV**: whole-word miss may silence; English should letter-fallback.
- NPU contention with YOLO/SCRFD/RKLLM; skip yolo infer during face dialogue helps.

---

## 10. Vision / LLM / gate

```text
Face dialogue Active/Grace + TTS active
  → ModelCoordinator::ShouldSkipYoloForDialogueTts() == true
  → Pipeline::RunEnabledSlots skips yolo infer (slots stay on)
  → scrfd stays → gate / Grace / Locked unchanged
```

**Forbidden**:

- TTS QoS causing `mode:none`;
- Disabling scrfd or faking person state;
- Disabling yolo in idle scene because of TTS (no person → vision unchanged).

---

## 11. Configuration

See [`config/default.yaml`](../runtime/config/default.yaml) → `model.tts`.

| Field | Meaning |
|-------|---------|
| `enabled` | Master switch (needs `model.llm.enabled`) |
| `skip_static_greeting` | `true` → no static greet |
| `encoder_path` / `decoder_path` | RKNN |
| `lexicon_path` / `tokens_path` | Lexicon |
| `language` | e.g. `ZH_MIX_EN` |
| `speak_id` / `speed` / `disable_bert` | Inference params |
| `preload_on_startup` | Async TTS load at startup (recommended `true`) |
| `max_speak_chars` | `0` = no truncate |
| `split_min_chars` | Melo internal min chars per split |
| `qos.enable_visual_throttle` | Skip yolo infer on low PCM watermark |
| `qos.low_watermark_chunks` / `high_watermark_chunks` | PCM queue thresholds |
| `qos.min_start_pcm_chunks` | Chunks before formal play starts |
| `planner.zh_min_chars` / `zh_max_chars` | Chinese emit thresholds |
| `planner.en_min_words` / `en_max_words` | English emit thresholds |
| `planner.fallback_timeout_ms` | Timeout emit when under max |

---

## 12. Logs and troubleshooting

| Log | Meaning |
|-----|---------|
| `first pcm enqueued in X ms` | First text → first PCM queued |
| `first pcm played in X ms` | First text → first PCM played |
| `pcm underrun #N` | Playback starvation (formal acceptance: 0) |
| `ModelCoordinator: tts yolo-skip on` | Skipping yolo infer during face dialogue |
| `mode:none` | **Abnormal** — check TTS QoS did not disable slots |

Debug order:

1. Short ack within ≤1s (FastAck cache ready);
2. Formal `underrun` count;
3. Decoder called too often (`inference_decoder_model` / gaps) → raise `planner.*` or check `CoalesceFormalAnswer`;
4. NPU contention (`tts yolo-skip on` during dialogue);
5. OOV swallowing (lexicon).

Platform TTS summary: [architecture-and-runtime.md](architecture-and-runtime.md) §8.

---

*If this doc disagrees with code, code wins; acceptance follows §2.*
