/*
 * voice/voice_output.h
 *
 * 门控层与出声实现之间的窄接口，避免 platform 依赖具体 TtsWorker。
 */
#pragma once

#include <string>

class IVoiceOutput {
public:
    virtual ~IVoiceOutput() = default;

    // 播报静态问候等一次性文本（不经 Planner）。
    virtual void PlayStaticText(const std::string& text) = 0;
    // 抢占：清空队列并升代际。
    virtual void Cancel() = 0;
    // 主线程轮询异步 Init 结果。
    virtual void PollInitState() = 0;
    // TTS 活跃且 PCM 低水位时建议视觉降载。
    virtual bool NeedPlaybackProtection() const = 0;
};
