# LLM 适配层（RKLLM）

> 代码：`runtime/adapters/llm/`。与 `adapters/yolo`、`adapters/scrfd` 同级，但 **不** 实现 `IModelAdapter` 每帧推理。平台总览见 [系统架构与运行逻辑.md](系统架构与运行逻辑.md)。

| 文件 | 职责 |
|------|------|
| `rkllm_session.*` | `rkllm_init` / **`rkllm_run`（同步）** / `rkllm_abort` / `rkllm_destroy`；回调 NORMAL 直写 stdout |
| `llm_worker.*` | 异步加载权重；`infer_thread_` 跑 `RunPromptSync`；`SubmitPrompt` 与排队 |

第三方：`runtime/3rdparty/rkllm/`（`rkllm.h`、`librkllmrt.so`、`libgomp.so`）。**勿修改 3rdparty 目录内容。**

配置：`config/default.yaml` 中 `model.llm.*`。板端需自备 `.rkllm`。

**两条输出路径：**

- **用户对话**：`SubmitPrompt` → `rkllm_run` → 流式 `AI>`（`StaticCallback`）。
- **自动问候**：`LlmGreeting` 直接 `SetBannerLine(auto_greeting_text)`，**不经** RKLLM。

挂钩：`platform/llm_greeting.cpp` ← `ModelCoordinator::UpdateAfterFrame` ← 人脸信号。

详见 [LLM与ModelCoordinator集成.md](LLM与ModelCoordinator集成.md)。
