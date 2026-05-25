/*
 * engine/logging.h
 *
 * 【platform 层】轻量调试日志（stdout/stderr）。
 * 用于 ModelCoordinator 切换、懒加载、信号合并等关键路径的可观测性。
 */
#pragma once

#include <algorithm>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <string>

enum class LogLevel {
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3
};

// 全局日志级别存储（header-only 方案）。
inline LogLevel& LogLevelStorage() {
    static LogLevel level = LogLevel::Info;
    return level;
}

// 设置日志级别（大小写不敏感）。
inline void SetLogLevelByName(const std::string& name) {
    std::string s = name;
    std::transform(
        s.begin(),
        s.end(),
        s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "debug") {
        LogLevelStorage() = LogLevel::Debug;
    } else if (s == "info" || s.empty()) {
        LogLevelStorage() = LogLevel::Info;
    } else if (s == "warn" || s == "warning") {
        LogLevelStorage() = LogLevel::Warn;
    } else if (s == "error") {
        LogLevelStorage() = LogLevel::Error;
    }
}

// 判断当前级别是否应输出。
inline bool ShouldLog(LogLevel level) {
    return static_cast<int>(LogLevelStorage()) >= static_cast<int>(level);
}

// 诊断日志统一走 stderr，stdout 留给 SYS/YOU/AI> 与 RKLLM 流式 printf。
inline FILE* DiagnosticStream() {
    return stderr;
}

// 统一写普通日志前缀与正文。
inline void LogWithPrefix(FILE* out, const char* prefix, const char* fmt, va_list args) {
    std::fprintf(out, "%s", prefix);
    std::vfprintf(out, fmt, args);
    std::fprintf(out, "\n");
    std::fflush(out);
}

// 会话平面输出：SYS/YOU/AI（总是输出）。
inline void LogSessionLine(const char* role, const char* fmt, va_list args) {
    std::fprintf(stdout, "%s ", role);
    std::vfprintf(stdout, fmt, args);
    std::fprintf(stdout, "\n");
    std::fflush(stdout);
}

// Info 级日志。
inline void LogInfo(const char* fmt, ...) {
    if (!ShouldLog(LogLevel::Info)) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    LogWithPrefix(DiagnosticStream(), "[INFO] ", fmt, args);
    va_end(args);
}

// Debug 级日志。
inline void LogDebug(const char* fmt, ...) {
    if (!ShouldLog(LogLevel::Debug)) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    LogWithPrefix(DiagnosticStream(), "[DBG] ", fmt, args);
    va_end(args);
}

// Warn 级日志。
inline void LogWarn(const char* fmt, ...) {
    if (!ShouldLog(LogLevel::Warn)) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    LogWithPrefix(DiagnosticStream(), "[WARN] ", fmt, args);
    va_end(args);
}

// Error 级日志。
inline void LogError(const char* fmt, ...) {
    if (!ShouldLog(LogLevel::Error)) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    LogWithPrefix(DiagnosticStream(), "[ERROR] ", fmt, args);
    va_end(args);
}

// Fatal 级日志（始终输出）。
inline void LogFatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogWithPrefix(DiagnosticStream(), "[FATAL] ", fmt, args);
    va_end(args);
}

// 会话系统提示（始终输出）。
inline void LogSystem(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogSessionLine("SYS>", fmt, args);
    va_end(args);
}

// 会话用户输入（始终输出）。
inline void LogUser(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogSessionLine("YOU>", fmt, args);
    va_end(args);
}

// 会话模型输出（始终输出）。
inline void LogAi(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogSessionLine("AI>", fmt, args);
    va_end(args);
}