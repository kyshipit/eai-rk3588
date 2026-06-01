🌐 Language: **English** | [中文](README_CN.md)

# RK3588 Edge AI Inference Platform

**Edge AI Runtime** on Rockchip RK3588 NPU (ALIENTEK and compatible boards, tuned via yaml). Main binary: **edgeai_platform_app**, driven by `runtime/config/default.yaml`. You need the `runtime/` build and **model files** under `model/` (`.rknn`, `.rkllm`, TTS lexicon/RKNN paths in yaml) on the board.

The **default reference app** below matches the current `default.yaml` (`model.llm.enabled` and `model.tts.enabled` are true): camera vision, face-gated on-device chat, and TTS—all on the board. Other products can disable LLM/TTS or change slot policy without changing the platform core.

## Default reference app

| Phase | What you see | What runs |
|-------|----------------|-----------|
| Startup | Preview window; `SYS>` loading / ready | Load yaml; optional RKLLM/TTS preload; sync YOLO init |
| Idle / person | Person boxes; face boxes when someone is present | Scene debounce idle→person; SCRFD slot on in person |
| Stable face | `AI>` greeting + speaker output | Static greeting via `SetBannerLine` + `PlayText` when `skip_static_greeting=false` |
| User types `YOU>` | Short ack sound → streaming `AI>` → spoken answer | FastAck cached PCM (`model.tts.fast_ack`, ≤1s) → RKLLM side path → MeloTTS streaming; requires **gst-launch-1.0** |
| Another `YOU>` | Previous speech stops; latest turn wins | `TtsWorker::Cancel` |
| Face leaves | May still accept input in Grace; then rejected | Locked / Grace state machine |
| Missing `.rkllm` | Preview only, no greeting or chat | Vision-only mode (`SYS>` notice) |
| Exit | Window closes | ESC / Ctrl+C; release camera and LLM/TTS |

Terminal: `SYS>` / `YOU>` / `AI>` on stdout; `[INFO]` and similar on stderr. Prefixes and yaml switches: [docs/README.md](docs/README.md).

## Architecture

![EdgeAI architecture](assets/architecture.svg)

*Diagram labels are in Chinese; directory paths match this repository.*

Solid lines: video frames and inference results. Dashed lines: YAML and person/face signals. **LLM and TTS are logic side paths** (`adapters/llm`, `adapters/tts`), not part of per-frame Preprocess→Inference→Postprocess.

| Layer | Directory | Role |
|-------|-----------|------|
| Entry | `runtime/app/` | Load YAML; start Pipeline and ModelCoordinator |
| Capture / display | `runtime/capture/` `runtime/display/` | Frames, rotation, overlays, OpenCV preview |
| Engine | `runtime/engine/` | Preprocess → inference → main-thread display and stdin |
| Policy | `runtime/platform/` | Scene switching, face gate, auto greeting |
| Models | `runtime/adapters/` | yolo / scrfd / llm / tts plugins, enabled on demand |

Startup order, threads, and design trade-offs: [docs/architecture-and-runtime.md](docs/architecture-and-runtime.md) (§5–7; complements the diagram above).

## Quick start

**Environment**: ALIENTEK RK3588, toolchain `/opt/atk-dlrk3588-toolchain`; place model files under `model/`.

```bash
cd runtime && ./build-linux.sh
cd install/rk3588_linux_aarch64/rknn_edgeai_platform
./edgeai_platform_app config/default.yaml
```

Adjust camera, model paths, and LLM/TTS switches in `config/default.yaml` for your board.

## Configuration

| Key | Effect |
|-----|--------|
| `model.llm.enabled` | Dialogue pipeline; vision-only if `.rkllm` is missing |
| `model.tts.enabled` | Speech output (still requires `model.llm.enabled` at startup) |
| `model.tts.skip_static_greeting` | Skip static greeting TTS after stable face when `true` |
| Model paths | `model.yolo.path`, `model.scrfd.path`, `model.llm.path`, `model.tts.*` |

See comments in `runtime/config/default.yaml`.

## Documentation

| Doc | Purpose |
|-----|---------|
| [docs/README.md](docs/README.md) | **Documentation index**, terminal conventions, topic navigation |
| [docs/architecture-and-runtime.md](docs/architecture-and-runtime.md) | Startup order, Pipeline, slots, platform design |
| [docs/tts-melotts.md](docs/tts-melotts.md) | TTS design and acceptance |
| [docs/llm-model-coordinator.md](docs/llm-model-coordinator.md) | RKLLM, gate, terminal UX |
| [docs/troubleshooting.md](docs/troubleshooting.md) | YOLO/SCRFD paths and RKNN outputs, exit/crash; TTS see TTS doc |

## Repository layout

```text
edgeai_platform/
├── model/          # yolov5.rknn, scrfd.rknn, .rkllm, TTS encoder/decoder RKNN, lexicon.txt, tokens.txt
├── docs/           # platform docs (entry: docs/README.md)
├── assets/         # architecture diagram, etc.
├── runtime/
│   ├── app/ engine/ platform/ capture/ display/
│   ├── adapters/yolo|scrfd|llm|tts/
│   └── config/default.yaml
└── verify/         # PC-side RKNN checks, not used on board
```

## License

MIT License
