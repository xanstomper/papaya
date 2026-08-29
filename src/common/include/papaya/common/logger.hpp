#pragma once

#include "papaya/common/types.hpp"
#include <string>
#include <string_view>
#include <format>
#include <iostream>
#include <mutex>

namespace papaya::log {

enum class Level {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical
};

class Logger {
public:
    static Logger& instance();

    void set_level(Level level);
    Level get_level() const;

    void log(Level level, std::string_view component, std::string_view message);

private:
    Logger() = default;
    Level current_level_{Level::Info};
    std::mutex mutex_;
};

template <typename... Args>
void info(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log(Level::Info, component, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void debug(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log(Level::Debug, component, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log(Level::Warn, component, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log(Level::Error, component, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void trace(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    Logger::instance().log(Level::Trace, component, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace papaya::log
