#include "papaya/common/logger.hpp"
#include <chrono>
#include <iomanip>

namespace papaya::log {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::set_level(Level level) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_level_ = level;
}

Level Logger::get_level() const {
    return current_level_;
}

void Logger::log(Level level, std::string_view component, std::string_view message) {
    if (level < current_level_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const char* level_str = "INFO";
    const char* color_code = "\033[0m";

    switch (level) {
        case Level::Trace:    level_str = "TRACE"; color_code = "\033[90m"; break;
        case Level::Debug:    level_str = "DEBUG"; color_code = "\033[36m"; break;
        case Level::Info:     level_str = "INFO "; color_code = "\033[32m"; break;
        case Level::Warn:     level_str = "WARN "; color_code = "\033[33m"; break;
        case Level::Error:    level_str = "ERROR"; color_code = "\033[31m"; break;
        case Level::Critical: level_str = "CRIT "; color_code = "\033[35;1m"; break;
    }

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
    localtime_r(&time, &tm_buf);

    std::cout << color_code << "[" 
              << std::put_time(&tm_buf, "%H:%M:%S") << "." 
              << std::setfill('0') << std::setw(3) << ms.count() << "]"
              << " [" << level_str << "]"
              << " [" << component << "] "
              << message << "\033[0m\n";
}

} // namespace papaya::log
