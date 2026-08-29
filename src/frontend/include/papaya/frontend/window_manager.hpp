#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <string>

namespace papaya::frontend {

struct WindowConfig {
    std::string title{"Project Papaya - ARM Steam Compatibility Layer"};
    u32 width{1920};
    u32 height{1080};
    bool fullscreen{false};
    bool headless{false};
    bool vsync{true};
};

class WindowManager {
public:
    explicit WindowManager(const WindowConfig& config = {});
    ~WindowManager();

    Result<> initialize();
    void poll_events();
    bool should_close() const { return should_close_; }
    void request_close() { should_close_ = true; }

    u32 get_width() const { return config_.width; }
    u32 get_height() const { return config_.height; }
    bool is_headless() const { return config_.headless; }

private:
    WindowConfig config_;
    bool should_close_{false};
    bool is_initialized_{false};
};

} // namespace papaya::frontend
