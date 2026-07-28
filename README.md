🌐 语言: **中文** | [English](README_EN.md)

# EAI-RK3588：基于插件的边缘AI平台

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

EAI-RK3588 是为 Rockchip RK3588 设计的可扩展边缘推理平台。通过一份 YAML 配置文件驱动，结合多线程视频流水线与插件化架构，内置 YOLO、SCRFD 模型，并支持本地 RKLLM 对话及语音合成（TTS）。默认应用展示了人脸门控、AI 问候与对话等功能。

<video src="https://github.com/user-attachments/assets/9cd2d897-d02b-4f66-a1cf-d374f2e17c5b" controls width="100%"></video>

---
## 📋 目录

- [✨ 核心特性](#-核心特性)
- [🏗️ 架构设计](#️-架构设计)
- [🚀 快速开始](#-快速开始)
- [⚙️ 配置说明](#️-配置说明)
- [📖 文档与代码入口](#-文档与代码入口)
- [🔧 本地检查工具](#-本地检查工具)
- [📄 License](#-license)
---

## ✨ 核心特性

- **插件化架构**：YOLO、SCRFD、LLM、TTS 均以插件形式实现，按需加载。

- **配置驱动**：所有功能（检测、对话、语音）通过 `default.yaml` 灵活开关。

- **低延迟流水线**：视频帧处理与 LLM/TTS 逻辑分离，保证实时推理性能。

- **开箱即用的人脸门控场景**：人员检测 → 人脸识别 → 自动问候 → 语音对话，完整端到端流程。

- **支持本地大模型**：集成 RKLLM，无需联网即可进行对话推理。

- **跨平台编译**：提供交叉编译脚本，快速部署到正点原子 RK3588 开发板。


## 🏗️ 架构设计

![Edge AI Runtime 架构](assets/architecture_cn.svg)

> 实线：视频帧与推理结果；虚线：YAML 与人体/人脸信号。LLM、TTS 均为逻辑旁路（`adapters/llm`、`voice/` + `adapters/melotts`），不参与每帧 `Preprocess→Inference→Postprocess` 流水线。

| 层       | 目录                                    | 职责                                      |
| -------- | --------------------------------------- | ----------------------------------------- |
| 入口     | `runtime/app/`                          | 读取 YAML，启动 Pipeline 与 ModelCoordinator |
| 采集/显示 | `runtime/capture/`, `runtime/display/`  | 采集帧、旋转、画框、OpenCV 预览           |
| 引擎     | `runtime/engine/`                       | 预处理 → 推理 → 主线程显示与 stdin 交互   |
| 策略     | `runtime/platform/`                     | 场景切换、人脸门控、自动问候逻辑          |
| 模型插件 | `runtime/adapters/`                     | yolo / scrfd / llm / tts 插件，按配置启停 |

更详细的启动顺序、线程模型与设计决策请参阅：[docs/architecture-and-runtime.md](docs/architecture-and-runtime.md)。

## 🚀 快速开始

```bash
# 1. 进入 runtime 目录
cd runtime

# 2. 执行交叉编译脚本（使用正点原子工具链）
./build-linux.sh

# 3. 将编译产物推送到开发板（替换 <target_directory> 为板端目录，如 /userdata）
adb push install/rk3588_linux_aarch64/rknn_eai_rk3588 /userdata/aidemo

# 4. 进入板端目录
cd /userdata/aidemo/rknn_eai_rk3588
./edgeai_app
```

## ⚙️ 配置说明

关键配置项（位于 `config/default.yaml`）：

| 配置项 | 说明 |
| ------ | ---- |
| `model.llm.enabled` | 是否启用 LLM 对话链路；若 `false` 或模型缺失，仅运行视觉检测 |
| `model.tts.enabled` | 是否启用 TTS 语音播报（需 `model.llm.enabled` 为 `true`） |
| `model.tts.skip_static_greeting` | `true` 时，人脸稳定后不播报静态问候语（只播报后续对话） |
| `model.yolo.path` | YOLO 模型文件路径（相对于可执行文件） |
| `model.scrfd.path` | SCRFD 人脸检测模型路径 |
| `model.llm.path` | RKLLM 模型文件路径 |
| `model.tts.encoder_path` / `decoder_path` | TTS 编码器/解码器 RKNN 路径 |
| `model.tts.lexicon_path` / `tokens_path` | TTS 词表文件路径 |
| `capture.device` | 摄像头设备节点（如 `/dev/video0`） |
| `display.window_name` | 预览窗口标题 |

完整字段及注释请参考 `runtime/config/default.yaml`。

---

## 📖 文档与代码入口

| 文档/用途 | 说明 |
| --------- | ---- |
| [docs/architecture-and-runtime.md](docs/architecture-and-runtime.md) | 启动顺序、流水线、槽位与平台设计详情 |
| [docs/tts-melotts.md](docs/tts-melotts.md) | TTS 实现细节与验收指南 |
| [docs/llm-model-coordinator.md](docs/llm-model-coordinator.md) | RKLLM 协调、门控逻辑与终端交互 UX |
| [docs/troubleshooting.md](docs/troubleshooting.md) | 常见问题排查（无框、路径错误、退出崩溃等） |
| [docs/adapters.md](docs/adapters.md) | 各插件模块（yolo/scrfd/llm/tts）的文件职责说明 |

**代码入口**：

| 用途 | 路径 |
| ---- | ---- |
| 主程序入口 | `runtime/app/main.cc` |
| 每帧处理流水线 | `runtime/engine/pipeline.cpp` |
| 场景协调器 | `runtime/platform/model_coordinator.cpp` |
| 人脸门控与问候逻辑 | `runtime/platform/llm_greeting.cpp` |
| RKLLM 插件 | `runtime/adapters/llm/` |
| TTS 实现 | `runtime/voice/` + `runtime/adapters/melotts/` |
| 默认配置文件 | `runtime/config/default.yaml` |

---

## 🔧 本地检查工具

在推送至板端前，可在仓库根目录运行以下脚本进行预检：

```bash
# 检查 default.yaml 字段类型与范围（不检查文件存在性）
python3 tools/check_config.py

# 检查 model/ 下各模型文件是否存在（若缺少 .rkllm 仅给出警告）
./tools/check_models.sh
```

## 仓库结构

```text
edgeai_platform/
├── model/          # yolov5.rknn、scrfd.rknn、.rkllm、TTS encoder/decoder RKNN、lexicon.txt、tokens.txt
├── docs/           # 平台文档
├── assets/         # 架构图等
├── runtime/
│   ├── app/ engine/ platform/ capture/ display/
│   ├── adapters/yolo|scrfd|llm|tts/
│   └── config/default.yaml
└── tools/          # 开发/集成辅助（配置校验、模型文件检查等），不参与板端运行
```

## 📄 License

MIT License
