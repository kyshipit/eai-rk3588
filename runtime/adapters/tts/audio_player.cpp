/*
 * adapters/tts/audio_player.cpp
 */
#include "audio_player.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "audio_utils.h"
#include "platform/logging.h"

namespace {
constexpr const char* kWavPath = "/tmp/edgeai_tts.wav";
std::atomic<pid_t> g_play_pid(0);
}  // namespace

// 终止正在播放的 gst-play-1.0 子进程。
void AudioPlayer::Stop() {
    const pid_t pid = g_play_pid.exchange(0);
    if (pid > 1) {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, WNOHANG);
    }
}

// 写 wav 后 fork+gst-play-1.0 阻塞至播完；可被 Stop 打断。
bool AudioPlayer::PlayPcm(const std::vector<float>& pcm, int sample_rate) {
    if (pcm.empty() || sample_rate <= 0) {
        return false;
    }
    Stop();
    const int ret =
        save_audio(kWavPath, const_cast<float*>(pcm.data()),
                   static_cast<int>(pcm.size()), sample_rate, 1);
    if (ret != 0) {
        LogWarn("AudioPlayer: save_audio failed ret=%d", ret);
        return false;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        LogWarn("AudioPlayer: fork failed");
        return false;
    }
    if (pid == 0) {
        (void)sample_rate;
        execlp("gst-play-1.0", "gst-play-1.0", "-q", "--no-interactive", kWavPath,
               static_cast<char*>(nullptr));
        _exit(127);
    }
    g_play_pid.store(pid);
    int status = 0;
    if (waitpid(pid, &status, 0) == pid) {
        g_play_pid.store(0);
        return true;
    }
    g_play_pid.store(0);
    return false;
}
