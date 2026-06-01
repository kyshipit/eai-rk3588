# Runtime 文档索引

板端开发与排障入口。仓库根 [README.md](../README.md) 为项目总览。

## 推荐阅读顺序

1. [系统架构与运行逻辑.md](系统架构与运行逻辑.md) — 平台主文档：分层、启动/运行时、设计取舍（LLM/TTS 细节见专文）
2. [TTS与MeloTTS集成说明.md](TTS与MeloTTS集成说明.md) — 改语音/TTS 时读（**唯一验收依据**）
3. [LLM与ModelCoordinator集成.md](LLM与ModelCoordinator集成.md) — 改对话/门控时读
4. [适配器说明.md](适配器说明.md) — 查 adapter 文件职责
5. [运行排障.md](运行排障.md) — 出问题时查

## 能力摘要

- **可执行文件**：`edgeai_platform_app`（`runtime/` 下 `./build-linux.sh`）
- **视觉槽**：YOLOv5（哨兵）、SCRFD（人脸）；`ModelCoordinator` 去抖切换
- **LLM**（`model.llm.enabled`）：自动问候为 yaml 静态文案 + `SetBannerLine`；终端 `YOU>` → `SubmitPrompt` → 独立线程 `rkllm_run`
- **TTS**（`model.tts.enabled`）：FastAck 短反馈 + 正式回答连续播；详见 TTS 主文档
- **配置**：`config/default.yaml` 为唯一默认来源
- **勿随意修改**：`runtime/3rdparty`、`runtime/utils` 为正点原子/RK 上游

## 非验收 backlog

- VAD/ASR/AEC：实现真实语音输入与打断
- 更快 TTS / 预合成话术：提升机器人语音体验
- YOLO-World：开放词汇视觉哨兵
- 按键输入：`LlmPromptSource::Button` 待接入

## License

MIT License
