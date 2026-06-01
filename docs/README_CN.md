Language: **中文** | [English](README.md)

# Edge AI Runtime 文档

> 板端 **RK3588** 上的可扩展推理平台：摄像头 → YOLO/SCRFD 视觉 →（可选）本地 RKLLM 对话 + MeloTTS 语音。  
> 仓库总览与硬件环境见根目录 **[README_CN.md](../README_CN.md)**。

---

## 概述

| 组件 | 作用 |
|------|------|
| **视觉** | 有人 → 开 SCRFD 画人脸框；场景由 `ModelCoordinator` 去抖切换 |
| **对话** | 人脸稳定 → 终端 `AI>` 问候；`YOU>` → RKLLM 流式回复（独立线程，不占每帧视觉） |
| **语音** | `YOU>` 后 FastAck 短反馈（≤1s）+ 正式回答 TTS（需 `gst-launch-1.0`） |
| **配置** | 唯一默认：**`runtime/config/default.yaml`** |

主程序：`edgeai_platform_app`（在 `runtime/` 编译生成）。

---

## 编译与运行

**前提**：正点原子 RK3588 工具链、`model/` 下已有 `yolov5.rknn`、`scrfd.rknn`；开对话则需 `.rkllm` 与 TTS 词表/RKNN（见 yaml 路径）。

```bash
cd runtime && ./build-linux.sh
cd install/rk3588_linux_aarch64/rknn_edgeai_platform
./edgeai_platform_app config/default.yaml
```

**终端约定**（会话走 stdout，诊断走 stderr）：

| 前缀 | 含义 |
|------|------|
| `SYS>` | 系统状态（加载中、仅视觉、门控拒绝等） |
| `YOU>` | 你的输入（stdin 一行） |
| `AI>` | 模型回复或静态问候 |

**常用开关**（改 yaml 后重启）：

```yaml
model.llm.enabled: true/false    # 对话链路
model.tts.enabled: true/false    # 语音（仍需 llm.enabled 以创建链路）
model.tts.skip_static_greeting: true   # 不播人脸稳定后的静态问候
```

仅视觉：缺 `.rkllm` 或 init 失败 → 预览正常，`SYS> 仅视觉模式…`，无问候、不接收 `YOU>`。

---

## 文档索引

| 主题 | 文档 | 说明 |
|------|------|------|
| 平台架构与运行逻辑 | [architecture-and-runtime_CN.md](architecture-and-runtime_CN.md) | 分层、槽、启动顺序、Pipeline 线程、设计取舍 |
| 语音 TTS | [tts-melotts_CN.md](tts-melotts_CN.md) | FastAck、Planner；**TTS 唯一验收依据**；断续/underrun/FastAck 见 §12 |
| 对话与门控 | [llm-model-coordinator_CN.md](llm-model-coordinator_CN.md) | RKLLM、门控状态机、终端 `YOU>` 数据流 |
| Adapter 源码 | [adapters_CN.md](adapters_CN.md) | `adapters/{yolo,scrfd,llm,tts}/` 文件职责 |
| 故障排查 | [troubleshooting_CN.md](troubleshooting_CN.md) | 0 框、路径错、退出/崩溃；TTS 细节见 TTS 专文 |

架构图（与根 [README_CN.md](../README_CN.md)、[architecture-and-runtime_CN.md](architecture-and-runtime_CN.md) §1 同源）：[`assets/architecture_cn.svg`](../assets/architecture_cn.svg)。

---

## 代码与配置入口

| 用途 | 路径 |
|------|------|
| 启动与读配置 | `runtime/app/main.cc` |
| 每帧流水线 | `runtime/engine/pipeline.cpp` |
| 视觉槽 / 场景 | `runtime/platform/model_coordinator.cpp` |
| 人脸门控 / 问候 | `runtime/platform/llm_greeting.cpp` |
| RKLLM | `runtime/adapters/llm/` |
| TTS | `runtime/adapters/tts/` |
| 默认配置 | `runtime/config/default.yaml` |

**勿随意改**：`runtime/3rdparty/`、`runtime/utils/`（正点原子 / RK 上游）。

---

## 规划项

- 真实麦克风 / VAD / ASR / 语音打断  
- 按键输入（`LlmPromptSource::Button`）  
- 更快 TTS 或 YOLO-World 等  

---

## License

MIT License
