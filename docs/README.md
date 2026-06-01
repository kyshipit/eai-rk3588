Language: **English** | [中文](README_CN.md)

# Edge AI Runtime documentation

> Extensible on-board inference on **RK3588**: camera → YOLO/SCRFD vision → (optional) local RKLLM dialogue + MeloTTS speech.  
> Repository overview and hardware: root **[README.md](../README.md)**.

---

## Overview

| Component | Role |
|-----------|------|
| **Vision** | Person present → enable SCRFD face boxes; scene debounced by `ModelCoordinator` |
| **Dialogue** | Stable face → terminal `AI>` greeting; `YOU>` → streaming RKLLM (dedicated thread, not per-frame vision) |
| **Speech** | After `YOU>`: FastAck (≤1s) + formal answer TTS (requires `gst-launch-1.0`) |
| **Config** | Default source: **`runtime/config/default.yaml`** |

Main binary: `edgeai_platform_app` (built under `runtime/`).

---

## Build and run

**Prerequisites**: ALIENTEK RK3588 toolchain; `yolov5.rknn` and `scrfd.rknn` under `model/`; for chat add `.rkllm` and TTS lexicon/RKNN (yaml paths).

```bash
cd runtime && ./build-linux.sh
cd install/rk3588_linux_aarch64/rknn_edgeai_platform
./edgeai_platform_app config/default.yaml
```

**Terminal conventions** (session on stdout, diagnostics on stderr):

| Prefix | Meaning |
|--------|---------|
| `SYS>` | System status (loading, vision-only, gate reject, etc.) |
| `YOU>` | Your input (one stdin line) |
| `AI>` | Model reply or static greeting |

**Common switches** (restart after yaml change):

```yaml
model.llm.enabled: true/false    # dialogue pipeline
model.tts.enabled: true/false    # speech (still needs llm.enabled at startup)
model.tts.skip_static_greeting: true   # skip static greeting TTS after stable face
```

Vision-only: missing `.rkllm` or init failure → preview OK, `SYS> 仅视觉模式…`, no greeting, no `YOU>`.

---

## Navigation

| Goal | Document |
|------|----------|
| **Understand the platform** | [architecture-and-runtime.md](architecture-and-runtime.md) |
| **Dialogue / gate / RKLLM** | [llm-model-coordinator.md](llm-model-coordinator.md) |
| **Speech / TTS acceptance** | [tts-melotts.md](tts-melotts.md) (**sole TTS acceptance doc**) |
| **What each adapter file does** | [adapters.md](adapters.md) |
| **Zero boxes, wrong path, hang, crash** | [troubleshooting.md](troubleshooting.md) |
| **TTS gaps, underrun, FastAck silent** | [tts-melotts.md](tts-melotts.md) §12 |

---

## Reading order

1. **[architecture-and-runtime.md](architecture-and-runtime.md)** — layers, slots, startup, Pipeline threads, trade-offs  
2. **[tts-melotts.md](tts-melotts.md)** — required for TTS work (FastAck, Planner, underrun acceptance)  
3. **[llm-model-coordinator.md](llm-model-coordinator.md)** — gate FSM, terminal UX, `YOU>` flow  
4. **[adapters.md](adapters.md)** — `adapters/{yolo,scrfd,llm,tts}/` file roles  
5. **[troubleshooting.md](troubleshooting.md)** — when something breaks  

Architecture diagram (same as root [README.md](../README.md) and [architecture-and-runtime.md](architecture-and-runtime.md) §1): [`assets/architecture.svg`](../assets/architecture.svg).

---

## Code and config entry points

| Purpose | Path |
|---------|------|
| Startup and config | `runtime/app/main.cc` |
| Per-frame pipeline | `runtime/engine/pipeline.cpp` |
| Vision slots / scenes | `runtime/platform/model_coordinator.cpp` |
| Face gate / greeting | `runtime/platform/llm_greeting.cpp` |
| RKLLM | `runtime/adapters/llm/` |
| TTS | `runtime/adapters/tts/` |
| Default config | `runtime/config/default.yaml` |

**Do not edit casually**: `runtime/3rdparty/`, `runtime/utils/` (upstream ALIENTEK / RK).

---

## Backlog

- Real microphone / VAD / ASR / barge-in  
- Button input (`LlmPromptSource::Button`)  
- Faster TTS or YOLO-World, etc.

---

## License

MIT License
