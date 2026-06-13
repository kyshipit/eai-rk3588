Language: **English** | [中文](adapters_CN.md)

# Adapters overview

Quick reference for vision and logic adapters; see topic docs for detail.

---

## YOLO (`adapters/yolo/`)

- ALIENTEK YOLOv5 three-head RKNN (`output num: 3`, `dims[1]=255`).
- Postprocess: `yolo_postprocess.cpp` decodes `output[0..2]` in a fixed loop.
- Troubleshooting: [troubleshooting.md](troubleshooting.md) § Vision models.

---

## SCRFD (`adapters/scrfd/`)

- Nine outputs (`score_*` / `bbox_*` / `kps_*`), grouped layout.
- Postprocess: `ResolveScrfdHeadOutputs` in `scrfd_postprocess.cpp`.
- Troubleshooting: [troubleshooting.md](troubleshooting.md) § Vision models.

---

## LLM (`adapters/llm/`)

Same tree level as `adapters/yolo` and `adapters/scrfd`, but **does not** implement per-frame `IModelAdapter` inference.

| File | Role |
|------|------|
| `rkllm_session.*` | `rkllm_init` / **sync `rkllm_run`** / `rkllm_abort` / `rkllm_destroy`; NORMAL callback writes stdout |
| `llm_worker.*` | Async load; `infer_thread_` runs `RunPromptSync`; `OnLlmChunk` posts TTS chunk events; main thread `PollDeferred` |

Config: `model.llm.*`. Integration: [llm-model-coordinator.md](llm-model-coordinator.md).

**Two output paths:**

- **User dialogue**: `SubmitPrompt` → `rkllm_run` → streaming `AI>` (thinking shown) → chunk events → TTS.
- **Auto greeting**: `LlmGreeting` → `SetBannerLine` (static yaml); no RKLLM.

---

## TTS / MeloTTS (`adapters/tts/`)

### Purpose

Synthesize **speakable `AI>` text** to audio (44100 Hz):

- Static greeting (`SetBannerLine` → `PlayText`)
- Formal answer after `YOU>` (`TtsPlanner` → `EnqueueFormalAnswer`; short answers as Static, long answers merge PCM per job)

Thinking visible on terminal; **not** sent to TTS.

### Modules

| File | Role |
|------|------|
| `tts_ingress.*` | Filter thinking/tags; normalize UTF-8 input |
| `tts_planner.*` | Stream-plan formal answer segments; zh/en emit thresholds |
| `tts_worker.*` | Static / FormalAnswer; short formal as Static; coalesce before synth; `generation_` preemption |
| `melotts_session.*` | RKNN synth; `SynthesizeTextStreaming` sentence-level PCM |
| `audio_player.*` | Persistent `gst-launch-1.0` pipe writing float32 PCM |
| `tts_text_sanitizer.*` | `max_speak_chars` truncation only |
| `lexicon.hpp` / `split.hpp` | Lexicon; English OOV letter fallback |

### Models and config

On board under `./model/`: `encoder-ZH_MIX_EN.rknn`, `decoder-ZH_MIX_EN.rknn`, `lexicon.txt`, `tokens.txt`.

YAML: `model.tts.*` (parallel to `model.llm`; startup still requires `model.llm.enabled: true`).

### Playback

`AudioPlayer` uses a persistent **`gst-launch-1.0`** pipe to stdin with float32 PCM (**not** per-clip `gst-play` on wav files).

Design and acceptance: [tts-melotts.md](tts-melotts.md).
