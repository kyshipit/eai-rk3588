/*
 * adapters/tts/audio_player.h — 单实例持续 PCM 播放器（GStreamer 管道常驻）。
 */
#pragma once

#include <vector>

class AudioPlayer {
public:
    // 将 float PCM 追加写入常驻播放管道；由底层音频时钟连续输出。
    bool PlayPcm(const std::vector<float>& pcm, int sample_rate);
    // 终止当前常驻播放子进程（若存在）并清理管道。
    void Stop();
};
