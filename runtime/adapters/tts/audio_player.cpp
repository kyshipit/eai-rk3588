/*
 * adapters/tts/audio_player.cpp
 */
#include "audio_player.h"

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/logging.h"

namespace {
constexpr int kChannels = 1;
std::atomic<pid_t> g_play_pid(0);
std::atomic<int> g_write_fd(-1);
std::atomic<int> g_sample_rate(0);
std::mutex g_pipe_mutex;

// 关闭全局写端 fd（若存在）。
void CloseWriteFdLocked() {
    const int fd = g_write_fd.exchange(-1);
    if (fd >= 0) {
        close(fd);
    }
}

// 回收已退出子进程，避免僵尸。
void ReapChildIfExitedLocked(pid_t pid) {
    if (pid <= 1) {
        return;
    }
    int status = 0;
    const pid_t ret = waitpid(pid, &status, WNOHANG);
    if (ret == pid) {
        g_play_pid.store(0);
    }
}

// 启动单实例常驻 gst-launch：stdin 持续接收 float32 PCM。
bool StartStreamProcessLocked(int sample_rate) {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
        LogWarn("AudioPlayer: pipe failed errno=%d", errno);
        return false;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        LogWarn("AudioPlayer: fork failed errno=%d", errno);
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        close(fds[1]);
        // 静默播放器子进程输出，避免终端闪烁进度行影响输入体验。
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        const std::string raw_caps =
            "audio/x-raw,format=F32LE,rate=" + std::to_string(sample_rate) +
            ",channels=" + std::to_string(kChannels) + ",layout=interleaved";
        execlp("gst-launch-1.0",
               "gst-launch-1.0",
               "-q",
               "fdsrc",
               "fd=0",
               "!",
               "queue",
               "!",
                raw_caps.c_str(),
               "!",
               "audioconvert",
               "!",
               "audioresample",
               "!",
               "autoaudiosink",
               static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[0]);
    g_play_pid.store(pid);
    g_write_fd.store(fds[1]);
    g_sample_rate.store(sample_rate);
    return true;
}

// 确保常驻播放进程健康；异常退出时自动重启。
bool EnsureStreamProcessLocked(int sample_rate) {
    pid_t pid = g_play_pid.load();
    ReapChildIfExitedLocked(pid);
    pid = g_play_pid.load();
    if (pid > 1 && g_write_fd.load() >= 0 && g_sample_rate.load() == sample_rate) {
        return true;
    }
    if (pid > 1) {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, WNOHANG);
        g_play_pid.store(0);
    }
    CloseWriteFdLocked();
    return StartStreamProcessLocked(sample_rate);
}

// 向管道完整写入一段 PCM（处理短写与中断）。
bool WriteAllLocked(const void* data, size_t size) {
    const char* ptr = static_cast<const char*>(data);
    size_t left = size;
    while (left > 0) {
        const int fd = g_write_fd.load();
        if (fd < 0) {
            return false;
        }
        const ssize_t n = write(fd, ptr, left);
        if (n > 0) {
            ptr += n;
            left -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}
}  // namespace

// 终止常驻播放子进程并清理管道句柄。
void AudioPlayer::Stop() {
    std::lock_guard<std::mutex> lock(g_pipe_mutex);
    CloseWriteFdLocked();
    const pid_t pid = g_play_pid.exchange(0);
    if (pid > 1 && kill(pid, SIGTERM) == 0) {
        waitpid(pid, nullptr, WNOHANG);
    }
    g_sample_rate.store(0);
}

// 将本段 float PCM 追加写入常驻管道，由单实例播放器连续输出。
bool AudioPlayer::PlayPcm(const std::vector<float>& pcm, int sample_rate) {
    if (pcm.empty() || sample_rate <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_pipe_mutex);
    if (!EnsureStreamProcessLocked(sample_rate)) {
        LogWarn("AudioPlayer: ensure stream process failed");
        return false;
    }
    const size_t bytes = pcm.size() * sizeof(float);
    if (WriteAllLocked(pcm.data(), bytes)) {
        return true;
    }
    LogWarn("AudioPlayer: write pipe failed, restart stream once");
    const pid_t pid = g_play_pid.exchange(0);
    if (pid > 1) {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, WNOHANG);
    }
    CloseWriteFdLocked();
    if (!StartStreamProcessLocked(sample_rate)) {
        return false;
    }
    return WriteAllLocked(pcm.data(), bytes);
}
