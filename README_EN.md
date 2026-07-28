🌐 Language: **English** | [中文](README.md)

# EAI-RK3588: Plugin-Based Edge AI Platform

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

EAI-RK3588 is an extensible edge inference platform designed for Rockchip RK3588. Driven by a single YAML configuration file, it combines a multi-threaded video pipeline with a plugin-based architecture. It includes built-in YOLO and SCRFD models, and supports local RKLLM dialogue and speech synthesis (TTS). The default application demonstrates face-gated access, AI greetings, and conversational dialogue.

<video src="https://github.com/user-attachments/assets/9cd2d897-d02b-4f66-a1cf-d374f2e17c5b" controls width="100%"></video>

---
## 📋 Table of Contents

- [✨ Core Features](#-core-features)
- [🏗️ Architecture](#️-architecture)
- [🚀 Quick Start](#-quick-start)
- [⚙️ Configuration](#️-configuration)
- [📖 Documentation & Code Entry Points](#-documentation--code-entry-points)
- [🔧 Local Check Tools](#-local-check-tools)
- [📄 License](#-license)
---

## ✨ Core Features

- **Plugin-based architecture**: YOLO, SCRFD, LLM, and TTS are all implemented as plugins, loaded on demand.

- **Configuration-driven**: All features (detection, dialogue, speech) can be toggled flexibly via `default.yaml`.

- **Low-latency pipeline**: Video frame processing is decoupled from LLM/TTS logic, ensuring real-time inference performance.

- **Ready-to-use face-gated scenario**: Person detection → face recognition → automatic greeting → voice conversation — a complete end-to-end pipeline.

- **Local LLM support**: Integrates RKLLM for on-device conversational inference without network connectivity.

- **Cross-compilation support**: Cross-compilation scripts are provided for rapid deployment to ALIENTEK RK3588 boards.

## 🏗️ Architecture

![Edge AI Runtime Architecture](assets/architecture.svg)

> Solid lines: video frames and inference results; dashed lines: YAML and person/face signals. LLM and TTS are logic side paths (`adapters/llm`, `voice/` + `adapters/melotts`), not part of the per-frame `Preprocess→Inference→Postprocess` pipeline.

| Layer           | Directory                               | Role                                                 |
| --------------- | --------------------------------------- | ---------------------------------------------------- |
| Entry           | `runtime/app/`                          | Reads YAML, starts Pipeline and ModelCoordinator     |
| Capture/Display | `runtime/capture/`, `runtime/display/`  | Frame capture, rotation, overlays, OpenCV preview    |
| Engine          | `runtime/engine/`                       | Preprocess → Inference → main-thread display & stdin |
| Policy          | `runtime/platform/`                     | Scene switching, face gate, auto-greeting logic      |
| Model Plugins   | `runtime/adapters/`                     | yolo / scrfd / llm / tts plugins, toggled by config  |

For detailed startup sequence, threading model, and design decisions, see: [docs/architecture-and-runtime_EN.md](docs/architecture-and-runtime_EN.md).

## 🚀 Quick Start

```bash
# 1. Enter the runtime directory
cd runtime

# 2. Run the cross-compilation script (using ALIENTEK toolchain)
./build-linux.sh

# 3. Push the build artifact to the board (replace <target_directory> with the board path, e.g. /userdata)
adb push install/rk3588_linux_aarch64/rknn_eai_rk3588 /userdata/aidemo

# 4. Enter the board directory and run
cd /userdata/aidemo/rknn_eai_rk3588
./edgeai_app
```

## ⚙️ Configuration

Key configuration items (located in `config/default.yaml`):

| Config Item | Description |
| ----------- | ----------- |
| `model.llm.enabled` | Enable LLM dialogue pipeline; if `false` or model missing, vision-only mode |
| `model.tts.enabled` | Enable TTS speech output (requires `model.llm.enabled` to be `true`) |
| `model.tts.skip_static_greeting` | When `true`, skip the static greeting TTS after face stabilization |
| `model.yolo.path` | Path to YOLO model file (relative to executable) |
| `model.scrfd.path` | Path to SCRFD face detection model |
| `model.llm.path` | Path to RKLLM model file |
| `model.tts.encoder_path` / `decoder_path` | TTS encoder/decoder RKNN paths |
| `model.tts.lexicon_path` / `tokens_path` | TTS lexicon file paths |
| `capture.device` | Camera device node (e.g., `/dev/video0`) |
| `display.window_name` | Preview window title |

For the full list of fields and comments, see `runtime/config/default.yaml`.

---

## 📖 Documentation & Code Entry Points

| Document | Description |
| -------- | ----------- |
| [docs/architecture-and-runtime_EN.md](docs/architecture-and-runtime_EN.md) | Startup sequence, pipeline, slots, and platform design details |
| [docs/tts-melotts_EN.md](docs/tts-melotts_EN.md) | TTS implementation details and acceptance guide |
| [docs/llm-model-coordinator_EN.md](docs/llm-model-coordinator_EN.md) | RKLLM coordination, gating logic, and terminal interaction UX |
| [docs/troubleshooting_EN.md](docs/troubleshooting_EN.md) | Troubleshooting common issues (no boxes, path errors, exit/crash, etc.) |
| [docs/adapters_EN.md](docs/adapters_EN.md) | File roles for each plugin module (yolo/scrfd/llm/tts) |

**Code Entry Points**:

| Purpose | Path |
| ------- | ---- |
| Main entry point | `runtime/app/main.cc` |
| Per-frame pipeline | `runtime/engine/pipeline.cpp` |
| Scene coordinator | `runtime/platform/model_coordinator.cpp` |
| Face gate & greeting logic | `runtime/platform/llm_greeting.cpp` |
| RKLLM plugin | `runtime/adapters/llm/` |
| TTS implementation | `runtime/voice/` + `runtime/adapters/melotts/` |
| Default configuration | `runtime/config/default.yaml` |

---

## 🔧 Local Check Tools

Before pushing to the board, run the following scripts from the repository root for pre-flight checks:

```bash
# Validate default.yaml field types and ranges (does not check file existence)
python3 tools/check_config.py

# Check if model files under model/ exist (missing .rkllm only triggers a warning)
./tools/check_models.sh
```

## Repository Structure

```text
edgeai_platform/
├── model/          # yolov5.rknn, scrfd.rknn, .rkllm, TTS encoder/decoder RKNN, lexicon.txt, tokens.txt
├── docs/           # platform documentation
├── assets/         # architecture diagrams, etc.
├── runtime/
│   ├── app/ engine/ platform/ capture/ display/
│   ├── adapters/yolo|scrfd|llm|tts/
│   └── config/default.yaml
└── tools/          # dev/integration helpers (config & model checks, etc.), not used on board
```

## 📄 License

MIT License
