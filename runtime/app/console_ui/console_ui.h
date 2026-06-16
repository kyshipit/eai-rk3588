/*
 * app/console_ui/console_ui.h
 *
 * 参考应用终端交互：非阻塞轮询 stdin，解析 YOU> 行协议并回调提交。
 */
#pragma once

#include <functional>
#include <string>

class ConsoleUi {
public:
    using SubmitHandler = std::function<bool(const std::string& line)>;

    // 注册用户输入提交回调（通常绑定 LlmGreeting::SubmitUserPrompt）。
    void SetSubmitHandler(SubmitHandler handler);

    // 启动时检查 stdin 是否为 TTY 并打日志。
    void LogAvailability() const;

    // 主循环每帧/空闲时调用：非阻塞读 stdin，遇换行提交整行。
    void Poll();

private:
    SubmitHandler submit_handler_;
    std::string input_buffer_;
};
