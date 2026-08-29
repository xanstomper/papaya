#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <string>
#include <string_view>
#include <functional>

namespace papaya::frontend {

struct WindowConfig {
    std::string title{"Project Papaya - Xbox One Emulator"};
    u32 width{1920};
    u32 height{1080};
    bool fullscreen{false};
    bool headless{false};
    bool vsync{true};
};

using KeyCallback = std::function<void(u32 keycode, bool pressed)>;
using CloseCallback = std::function<void()>;

class WindowManager {
public:
    explicit WindowManager(const WindowConfig& config = {});
    ~WindowManager();

    Result<> initialize();
    void poll_events();
    bool should_close() const { return should_close_; }
    void request_close() { should_close_ = true; }

    void set_title(std::string_view title);
    void set_key_callback(KeyCallback cb) { key_cb_ = std::move(cb); }
    void set_close_callback(CloseCallback cb) { close_cb_ = std::move(cb); }

    u32 get_width() const { return width_; }
    u32 get_height() const { return height_; }
    bool is_headless() const { return config_.headless; }

private:
    WindowConfig config_;
    u32 width_{1920};
    u32 height_{1080};
    bool should_close_{false};
    bool is_initialized_{false};

    KeyCallback key_cb_;
    CloseCallback close_cb_;
};

} // namespace papaya::frontend
