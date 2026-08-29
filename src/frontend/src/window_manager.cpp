#include "papaya/frontend/window_manager.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::frontend {

WindowManager::WindowManager(const WindowConfig& config)
    : config_(config) {}

WindowManager::~WindowManager() = default;

Result<> WindowManager::initialize() {
    log::info("WINDOW", "Initializing Display Server Window [{}x{}, Headless: {}, Title: '{}']",
              config_.width, config_.height, config_.headless ? "YES" : "NO", config_.title);
    is_initialized_ = true;
    return {};
}

void WindowManager::poll_events() {
    // Process input / display events
}

} // namespace papaya::frontend
