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
    log::info("WINDOW", "Initializing Display Server Window [{}x{}, Headless: {}, Title: '{}']",
              config_.width, config_.height, config_.headless ? "YES" : "NO", config_.title);

    if (config_.headless) {
        is_initialized_ = true;
        return {};
    }

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        log::warn("WINDOW", "XOpenDisplay failed on DISPLAY='{}' - falling back to headless mode",
                  getenv("DISPLAY") ? getenv("DISPLAY") : "(null)");
        config_.headless = true;
        is_initialized_ = true;
        return {};
    }

    display_ = dpy;
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    unsigned long black = BlackPixel(dpy, screen);
    unsigned long white = WhitePixel(dpy, screen);

    u32 win_w = config_.width > 0 ? config_.width : 1280;
    u32 win_h = config_.height > 0 ? config_.height : 720;

    Window win = XCreateSimpleWindow(dpy, root, 100, 100, win_w, win_h, 1, white, black);
    if (!win) {
        log::error("WINDOW", "Failed to create X11 window!");
        return ErrorCode::UnsupportedOperation;
    }

    window_ = static_cast<u64>(win);

    // Set Window Title
    XStoreName(dpy, win, config_.title.c_str());

    // Select Events
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | KeyReleaseMask |
                           ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                           StructureNotifyMask | FocusChangeMask);

    // Setup WM_DELETE_WINDOW protocol
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    wm_delete_window_ = static_cast<u64>(wm_delete);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    // Map and raise window on screen
    XMapRaised(dpy, win);
    XFlush(dpy);

    log::info("WINDOW", "Successfully mapped X11 display window [ID: 0x{:X}] on screen", window_);
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
