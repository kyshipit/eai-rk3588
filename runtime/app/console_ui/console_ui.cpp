/*
 * app/console_ui/console_ui.cpp — 终端 YOU> 行输入
 */
#include "console_ui.h"

#include <cctype>
#include <cerrno>
#include <sys/select.h>
#include <unistd.h>

#include "platform/logging.h"

// 绑定用户输入提交逻辑（由 main 注入 LlmGreeting）。
void ConsoleUi::SetSubmitHandler(SubmitHandler handler) {
    submit_handler_ = std::move(handler);
}

// 非 TTY 时提示 YOU> 可能不可用，避免误以为程序无响应。
void ConsoleUi::LogAvailability() const {
    if (!::isatty(STDIN_FILENO)) {
        LogWarn("ConsoleUi: stdin is not a TTY, YOU> input may be unavailable");
    } else {
        LogDebug("ConsoleUi: stdin is a TTY");
    }
}

// 非阻塞 select+read，行缓冲后 trim 并调用 submit_handler_。
void ConsoleUi::Poll() {
    if (!submit_handler_) {
        return;
    }

    const int input_fd = STDIN_FILENO;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(input_fd, &rfds);
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    const int ret = select(input_fd + 1, &rfds, nullptr, nullptr, &tv);
    if (ret <= 0 || !FD_ISSET(input_fd, &rfds)) {
        return;
    }

    char buf[256];
    const ssize_t n = ::read(input_fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            LogWarn("ConsoleUi: prompt input read failed errno=%d", errno);
        }
        return;
    }

    for (ssize_t i = 0; i < n; ++i) {
        const char ch = buf[i];
        if (ch == '\r' || ch == '\n') {
            std::string line = input_buffer_;
            input_buffer_.clear();
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
                line.pop_back();
            }
            size_t start = 0;
            while (start < line.size() &&
                   std::isspace(static_cast<unsigned char>(line[start]))) {
                ++start;
            }
            if (start > 0) {
                line.erase(0, start);
            }
            if (line.empty()) {
                continue;
            }

            LogUser("%s", line.c_str());
            LogDebug("ConsoleUi: terminal prompt submitting (%zu chars)", line.size());
            const bool accepted = submit_handler_(line);
            if (!accepted) {
                LogDebug("ConsoleUi: terminal prompt rejected");
            }
            continue;
        }

        if (ch == '\b' || static_cast<unsigned char>(ch) == 0x7f) {
            if (!input_buffer_.empty()) {
                input_buffer_.pop_back();
            }
            continue;
        }

        const unsigned char uch = static_cast<unsigned char>(ch);
        if (ch == '\t' || uch >= 0x20) {
            input_buffer_.push_back(ch);
        }
    }
}
