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

## Documentation index

| Topic | Document | Notes |
|-------|----------|-------|
| Platform architecture and runtime | [architecture-and-runtime.md](architecture-and-runtime.md) | Layers, slots, startup order, Pipeline threads, trade-offs |
| Speech / TTS | [tts-melotts.md](tts-melotts.md) | FastAck, Planner; **sole TTS acceptance doc**; gaps/underrun/FastAck see §12 |
| Dialogue and face gate | [llm-model-coordinator.md](llm-model-coordinator.md) | RKLLM, gate FSM, terminal `YOU>` flow |
| Adapter source | [adapters.md](adapters.md) | `adapters/{yolo,scrfd,llm,tts}/` file roles |
| Troubleshooting | [troubleshooting.md](troubleshooting.md) | Zero boxes, wrong paths, exit/crash; TTS details in TTS doc |

Architecture diagram ([architecture-and-runtime.md](architecture-and-runtime.md) §1; also in root [README.md](../README.md)): [`assets/architecture.svg`](../assets/architecture.svg). (Chinese diagram: [`architecture_cn.svg`](../assets/architecture_cn.svg).)

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
