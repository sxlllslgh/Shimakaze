#pragma once

#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace shimakaze {

enum class LogLevel {
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
    off = 5,
};

class Logger {
public:
    static Logger& instance();

    void set_quiet(bool quiet) noexcept;
    void set_level(std::string_view level);
    void set_level(LogLevel level) noexcept;
    void set_file(const std::string& path);

    template <typename... Args>
    void trace(Args&&... args)
    {
        write(LogLevel::trace, "TRACE", std::forward<Args>(args)...);
    }

    template <typename... Args>
    void debug(Args&&... args)
    {
        write(LogLevel::debug, "DEBUG", std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(Args&&... args)
    {
        write(LogLevel::info, "INFO", std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(Args&&... args)
    {
        write(LogLevel::warn, "WARN", std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(Args&&... args)
    {
        write(LogLevel::error, "ERROR", std::forward<Args>(args)...);
    }

    template <typename... Args>
    void verbose(Args&&... args)
    {
        if (!quiet_) {
            write(LogLevel::info, "INFO", std::forward<Args>(args)...);
        }
    }

private:
    Logger() = default;

    template <typename... Args>
    void write(LogLevel level, std::string_view label, Args&&... args)
    {
        if (level < level_) {
            return;
        }
        std::lock_guard lock(mutex_);
        auto& out = file_ ? static_cast<std::ostream&>(*file_) : std::cerr;
        out << timestamp() << " [" << label << "] ";
        (out << ... << std::forward<Args>(args));
        out << '\n';
        out.flush();
    }

    static std::string timestamp();

    bool quiet_ = false;
    LogLevel level_ = LogLevel::info;
    std::unique_ptr<std::ofstream> file_;
    std::mutex mutex_;
};

} // namespace shimakaze
