# MeloTTS 适配器

> 代码：`runtime/adapters/tts/`。平台总览见 [系统架构与运行逻辑.md](系统架构与运行逻辑.md)。集成细节见 [TTS与MeloTTS集成讨论总结.md](TTS与MeloTTS集成讨论总结.md)。

## 作用

将终端 **`AI>` 助手话术** 合成为语音（44100Hz），包括：

- 静态自动问候（`SetBannerLine`，可 `skip_static_greeting` 关闭播报）
- `YOU>` 后 RKLLM 整轮回复（`reply_accumulator` 与 stdout 同源）

## 模型与配置

板端 `./model/`：

- `encoder-ZH_MIX_EN.rknn`、`decoder-ZH_MIX_EN.rknn`
- `lexicon.txt`、`tokens.txt`

YAML：`config/default.yaml` → `model.llm.tts.*`（需 `model.llm.enabled: true`）。

## 播放

合成后写 `/tmp/edgeai_tts.wav`，程序内由 `AudioPlayer` 调用：

```bash
gst-play-1.0 -q --no-interactive /tmp/edgeai_tts.wav
```

板端需已安装 `gst-play-1.0`。
