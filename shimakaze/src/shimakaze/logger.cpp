#include "shimakaze/logger.hpp"

#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace shimakaze {

Logger& Logger::instance()
{
    static Logger logger;
    return logger;
}

void Logger::set_quiet(bool quiet) noexcept
{
    quiet_ = quiet;
}

void Logger::set_level(std::string_view level)
{
    std::string normalized;
    normalized.reserve(level.size());
    for (const char ch : level) {
        if (ch != '-' && ch != '_') {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }

    if (normalized == "trace") {
        set_level(LogLevel::trace);
    } else if (normalized == "debug") {
        set_level(LogLevel::debug);
    } else if (normalized == "info") {
        set_level(LogLevel::info);
    } else if (normalized == "warn" || normalized == "warning") {
        set_level(LogLevel::warn);
    } else if (normalized == "error") {
        set_level(LogLevel::error);
    } else if (normalized == "off" || normalized == "none" || normalized == "silent") {
        set_level(LogLevel::off);
    } else {
        throw std::runtime_error("invalid loglevel: " + std::string(level));
    }
}

void Logger::set_level(LogLevel level) noexcept
{
    level_ = level;
}

void Logger::set_file(const std::string& path)
{
    if (path.empty()) {
        file_.reset();
        return;
    }

    auto stream = std::make_unique<std::ofstream>(path, std::ios::app);
    if (!*stream) {
        throw std::runtime_error("failed to open log file: " + path);
    }
    file_ = std::move(stream);
}

std::string Logger::timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);

    std::tm tm {};
#if defined(_WIN32)
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif

    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return os.str();
}

} // namespace shimakaze
