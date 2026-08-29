// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 KY (kyshipit)

/*
 * voice/audio_player.cpp
 */
#include "audio_player.h"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "platform/logging.h"

namespace {
constexpr int kChannels = 1;
constexpr size_t kColdPrimeSamples = 2205;
constexpr int kKeepaliveIntervalMs = 100;
constexpr size_t kKeepaliveChunkSamples = 1764;
constexpr int kKeepaliveMaxMs = 120000;
std::atomic<pid_t> g_play_pid(0);
std::atomic<int> g_write_fd(-1);
std::atomic<int> g_sample_rate(0);
std::atomic<bool> g_stream_needs_cold_prime(false);
std::atomic<bool> g_have_written_pcm(false);
std::atomic<bool> g_keepalive_stop{true};
std::atomic<bool> g_keepalive_paused{false};
std::mutex g_pipe_mutex;
std::thread g_keepalive_thread;

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
    g_stream_needs_cold_prime.store(true);
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

// 仅在新 gst 管道冷启动时灌短静音；已禁用 8820 idle priming（段间可闻停顿）。
bool PrimeColdStartIfNeededLocked() {
    if (!g_stream_needs_cold_prime.load()) {
        return true;
    }
    g_stream_needs_cold_prime.store(false);
    const std::vector<float> prime(kColdPrimeSamples, 0.0f);
    if (!WriteAllLocked(prime.data(), prime.size() * sizeof(float))) {
        return false;
    }
    LogInfo("AudioPlayer: cold primed stream (%zu samples)", kColdPrimeSamples);
    return true;
}

// 合成/播放空档：周期性微量静音维持管道，不触发长段 priming。
void IdleKeepaliveLoop(int sample_rate) {
    const auto started = std::chrono::steady_clock::now();
    while (!g_keepalive_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kKeepaliveIntervalMs));
        if (g_keepalive_stop.load() || g_keepalive_paused.load()) {
            continue;
        }
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
        if (elapsed_ms >= kKeepaliveMaxMs) {
            LogInfo("AudioPlayer: idle keepalive stopped after %d ms", kKeepaliveMaxMs);
            break;
        }
        std::lock_guard<std::mutex> lock(g_pipe_mutex);
        if (g_keepalive_stop.load() || g_keepalive_paused.load()) {
            continue;
        }
        if (!EnsureStreamProcessLocked(sample_rate)) {
            continue;
        }
        if (!PrimeColdStartIfNeededLocked()) {
            continue;
        }
        const std::vector<float> pad(kKeepaliveChunkSamples, 0.0f);
        WriteAllLocked(pad.data(), pad.size() * sizeof(float));
    }
}
}  // namespace

// 终止常驻播放子进程并清理管道句柄。
void AudioPlayer::Stop() {
    EndIdleKeepalive();
    std::lock_guard<std::mutex> lock(g_pipe_mutex);
    CloseWriteFdLocked();
    const pid_t pid = g_play_pid.exchange(0);
    if (pid > 1 && kill(pid, SIGTERM) == 0) {
        waitpid(pid, nullptr, WNOHANG);
    }
    g_sample_rate.store(0);
    g_stream_needs_cold_prime.store(false);
    g_have_written_pcm.store(false);
}

// 合成空档维持 gst 写入；已在跑则不再重启线程。
void AudioPlayer::BeginIdleKeepalive(int sample_rate) {
    if (sample_rate <= 0) {
        return;
    }
    if (g_keepalive_thread.joinable() && !g_keepalive_stop.load()) {
        return;
    }
    EndIdleKeepalive();
    g_keepalive_stop.store(false);
    g_keepalive_paused.store(false);
    g_keepalive_thread = std::thread(IdleKeepaliveLoop, sample_rate);
    LogInfo("AudioPlayer: idle keepalive started (interval=%dms)", kKeepaliveIntervalMs);
}

// 停止 keepalive 线程（Cancel/Shutdown）。
void AudioPlayer::EndIdleKeepalive() {
    g_keepalive_stop.store(true);
    g_keepalive_paused.store(false);
    if (g_keepalive_thread.joinable()) {
        g_keepalive_thread.join();
    }
}

// 将本段 float PCM 写入管道；暂停 keepalive 写同一 fd，避免 join 空窗与段间长静音。
bool AudioPlayer::PlayPcm(const std::vector<float>& pcm, int sample_rate) {
    if (pcm.empty() || sample_rate <= 0) {
        return false;
    }
    g_keepalive_paused.store(true);
    std::lock_guard<std::mutex> lock(g_pipe_mutex);
    if (!EnsureStreamProcessLocked(sample_rate)) {
        g_keepalive_paused.store(false);
        LogWarn("AudioPlayer: ensure stream process failed");
        return false;
    }
    if (!PrimeColdStartIfNeededLocked()) {
        g_keepalive_paused.store(false);
        LogWarn("AudioPlayer: stream prime failed, restart once");
        const pid_t pid = g_play_pid.exchange(0);
        if (pid > 1) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, WNOHANG);
        }
        CloseWriteFdLocked();
        if (!StartStreamProcessLocked(sample_rate) || !PrimeColdStartIfNeededLocked()) {
            return false;
        }
    }
    const size_t bytes = pcm.size() * sizeof(float);
    if (WriteAllLocked(pcm.data(), bytes)) {
        g_have_written_pcm.store(true);
        g_keepalive_paused.store(false);
        return true;
    }
    LogWarn("AudioPlayer: write pipe failed, restart stream once");
    const pid_t pid = g_play_pid.exchange(0);
    if (pid > 1) {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, WNOHANG);
    }
    CloseWriteFdLocked();
    if (!StartStreamProcessLocked(sample_rate) || !PrimeColdStartIfNeededLocked()) {
        g_keepalive_paused.store(false);
        return false;
    }
    if (!WriteAllLocked(pcm.data(), bytes)) {
        g_keepalive_paused.store(false);
        return false;
    }
    g_have_written_pcm.store(true);
    g_keepalive_paused.store(false);
    return true;
}
