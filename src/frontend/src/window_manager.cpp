#include "papaya/frontend/window_manager.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::frontend {

WindowManager::WindowManager(const WindowConfig& config)
    : config_(config), width_(config.width), height_(config.height) {}

WindowManager::~WindowManager() = default;

Result<> WindowManager::initialize() {
    log::info("FRONTEND", "Initializing Display Window ({}x{}, Headless: {})",
              width_, height_, config_.headless);
    is_initialized_ = true;
    return {};
}

void WindowManager::poll_events() {
    if (!is_initialized_) return;
    // Process window system events
}

void WindowManager::set_title(std::string_view title) {
    config_.title = title;
    log::debug("FRONTEND", "Window title updated: '{}'", config_.title);
}

} // namespace papaya::frontend
