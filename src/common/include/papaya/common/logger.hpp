#pragma once

#include <string_view>
#include <format>
#include <iostream>
#include <mutex>
#include <chrono>
#include <ctime>

namespace papaya::log {

enum class Level {
    Trace,
    Debug,
    Info,
    Warn,
    Error
};

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void set_level(Level lvl) { current_level_ = lvl; }
    Level get_level() const { return current_level_; }

    template <typename... Args>
    void log_msg(Level lvl, std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
        if (lvl < current_level_) return;

        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#if defined(_WIN32)
        localtime_s(&tm_buf, &now_c);
#else
        localtime_r(&now_c, &tm_buf);
#endif
        char time_str[32];
        std::strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm_buf);

        std::string msg = std::format(fmt, std::forward<Args>(args)...);

        const char* color = "\033[0m";
        const char* lvl_str = "INFO";

        switch (lvl) {
            case Level::Trace: color = "\033[90m"; lvl_str = "TRACE"; break;
            case Level::Debug: color = "\033[36m"; lvl_str = "DEBUG"; break;
            case Level::Info:  color = "\033[32m"; lvl_str = "INFO"; break;
            case Level::Warn:  color = "\033[33m"; lvl_str = "WARN"; break;
            case Level::Error: color = "\033[31;1m"; lvl_str = "ERROR"; break;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << color << "[" << time_str << "] [" << lvl_str << "] [" << tag << "] "
                  << msg << "\033[0m\n";
    }

private:
    Logger() = default;
    Level current_level_{Level::Info};
    std::mutex mutex_;
};

template <typename... Args>
inline void trace(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log_msg(Level::Trace, tag, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void debug(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log_msg(Level::Debug, tag, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void info(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log_msg(Level::Info, tag, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void warn(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log_msg(Level::Warn, tag, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void error(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log_msg(Level::Error, tag, fmt, std::forward<Args>(args)...);
}

} // namespace papaya::log
