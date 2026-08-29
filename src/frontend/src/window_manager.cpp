#include "papaya/frontend/window_manager.hpp"
#include "papaya/common/logger.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <cstdlib>

namespace papaya::frontend {

WindowManager::WindowManager(const WindowConfig& config)
    : config_(config) {}

WindowManager::~WindowManager() {
    if (display_) {
        Display* dpy = static_cast<Display*>(display_);
        if (window_) {
            XDestroyWindow(dpy, static_cast<Window>(window_));
            window_ = 0;
        }
        XCloseDisplay(dpy);
        display_ = nullptr;
    }
}

Result<> WindowManager::initialize() {
    log::info("WINDOW", "Initializing Display Server Window Manager [Headless: {}, Title: '{}']",
              config_.headless ? "YES" : "NO", config_.title);
    is_initialized_ = true;
    return {};
}

void WindowManager::poll_events() {
    if (!display_ || config_.headless) return;

    Display* dpy = static_cast<Display*>(display_);
    while (XPending(dpy) > 0) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == ClientMessage) {
            if (static_cast<u64>(ev.xclient.data.l[0]) == wm_delete_window_) {
                log::info("WINDOW", "Window close requested by user.");
                request_close();
            }
        } else if (ev.type == DestroyNotify) {
            request_close();
        }
    }
}

} // namespace papaya::frontend
