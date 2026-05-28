/*
 * adapters/tts/audio_player.h — PCM 落盘为 /tmp/edgeai_tts.wav，gst-play-1.0 播放。
 */
#pragma once

#include <vector>

class AudioPlayer {
public:
    // 将 float PCM 写入 wav 并阻塞播放；播放前会终止上一轮子进程。
    bool PlayPcm(const std::vector<float>& pcm, int sample_rate);
    // 终止当前 gst-play-1.0 子进程（若存在）。
    void Stop();
};
