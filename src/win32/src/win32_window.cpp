#include "papaya/win32/win32_window.hpp"
#include "papaya/common/logger.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cstring>
#include <cstdlib>

namespace papaya::win32 {

// ---------------------------------------------------------------------------
// Singleton manager instance
// ---------------------------------------------------------------------------
static X11WindowManager g_window_mgr;
X11WindowManager& window_manager() { return g_window_mgr; }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
X11WindowManager::X11WindowManager() = default;
X11WindowManager::~X11WindowManager() { shutdown(); }

bool X11WindowManager::initialize() {
    if (initialized_) return true;
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
        log::warn("WINDOW", "X11 display unavailable (DISPLAY={}) — window creation disabled",
                  getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        initialized_ = false;
        return false;
    }
    initialized_ = true;
    log::info("WINDOW", "X11 window manager initialized (server: {})", DisplayString(display_));
    return true;
}

void X11WindowManager::shutdown() {
    for (auto& [k, w] : windows_) {
        if (w->xid && display_) XDestroyWindow(display_, static_cast<Window>(w->xid));
        if (w->gc && display_) XFreeGC(display_, w->gc);
    }
    windows_.clear();
    if (display_) { XCloseDisplay(display_); display_ = nullptr; }
    initialized_ = false;
}

// ---------------------------------------------------------------------------
// Window classes
// ---------------------------------------------------------------------------
void* X11WindowManager::register_class(const char* name, void* wndproc, void* hinstance) {
    if (!name || !wndproc) return nullptr;
    auto& c = classes_[std::string(name)];
    c.name = name;
    c.window_proc = wndproc;
    c.instance_handle = hinstance;
    c.registered = true;
    return &c;   // ATOM / class handle
}

void* X11WindowManager::find_class(const char* name) {
    if (!name) return nullptr;
    auto it = classes_.find(std::string(name));
    return it == classes_.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// Window creation
// ---------------------------------------------------------------------------
void* X11WindowManager::create_window_ex(const char* class_name, const char* title, u32 style,
                                         int x, int y, int w, int h, void* parent,
                                         void* instance, void* param, bool hide) {
    (void)parent; (void)instance; (void)param;
    if (!initialized_ && !initialize()) return nullptr;

    auto* cls = static_cast<NativeWindowClass*>(find_class(class_name));
    if (!cls) {
        register_class(class_name ? class_name : "PapayaDefaultWindow", nullptr, instance);
        cls = static_cast<NativeWindowClass*>(find_class(class_name ? class_name : "PapayaDefaultWindow"));
    }

    auto win = std::make_unique<NativeWindow>();
    win->cls = cls;
    std::string final_title = (title && std::strlen(title) > 0) ? title : (class_name ? class_name : "Papaya Game");
    win->title = final_title;
    win->display = display_;
    win->style = style;

    // Multi-monitor aware placement: place on primary screen (0..1920)
    int scr_w = DisplayWidth(display_, DefaultScreen(display_));
    int scr_h = DisplayHeight(display_, DefaultScreen(display_));
    if (scr_w > 1920) scr_w = 1920; // Default bounds to primary display

    int default_w = (w > 0 && w < 10000 && w != static_cast<int>(0x80000000)) ? w : 1280;
    int default_h = (h > 0 && h < 10000 && h != static_cast<int>(0x80000000)) ? h : 720;
    int default_x = (x >= 0 && x < scr_w && x != static_cast<int>(0x80000000)) ? x : std::max(0, (scr_w - default_w) / 2);
    int default_y = (y >= 0 && y < scr_h && y != static_cast<int>(0x80000000)) ? y : std::max(0, (scr_h - default_h) / 2);

    win->width  = default_w;
    win->height = default_h;
    win->x      = default_x;
    win->y      = default_y;

    XSetWindowAttributes attrs{};
    attrs.border_pixel = 0;
    attrs.background_pixel = BlackPixel(display_, DefaultScreen(display_));
    attrs.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                       StructureNotifyMask | FocusChangeMask;

    Window xw = XCreateWindow(display_, DefaultRootWindow(display_),
                              win->x, win->y, win->width, win->height, 0,
                              CopyFromParent, InputOutput, CopyFromParent,
                              CWBackPixel | CWBorderPixel | CWEventMask, &attrs);
    if (!xw) { log::error("WINDOW", "XCreateWindow failed"); return nullptr; }

    XStoreName(display_, xw, win->title.c_str());
    Atom utf8_string = XInternAtom(display_, "UTF8_STRING", False);
    Atom net_wm_name = XInternAtom(display_, "_NET_WM_NAME", False);
    XChangeProperty(display_, xw, net_wm_name, utf8_string, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(win->title.c_str()), win->title.length());

    XClassHint class_hint;
    char res_name[] = "papaya";
    char res_class[] = "Papaya";
    class_hint.res_name = res_name;
    class_hint.res_class = res_class;
    XSetClassHint(display_, xw, &class_hint);

    Atom wm_protocols = XInternAtom(display_, "WM_PROTOCOLS", False);
    Atom wm_delete = XInternAtom(display_, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display_, xw, &wm_delete, 1);

    win->xid = static_cast<std::uint64_t>(xw);
    win->gc  = XCreateGC(display_, xw, 0, nullptr);
    win->hwnd = win.get();

    void* hwnd = win.get();
    const char* w_title = win->title.c_str();
    const int   w_w = win->width, w_h = win->height;
    windows_[hwnd] = std::move(win);

    log::info("WINDOW", "Created native window '{}' [{}x{}] hwnd={} xid=0x{:X}",
              w_title, w_w, w_h,
              reinterpret_cast<u64>(hwnd), static_cast<unsigned long long>(xw));

    // Post Win32 window creation messages to the queue so they are delivered
    // when the guest calls PeekMessage/GetMessage in its own message loop.
    // Do NOT dispatch synchronously — Godot's WndProc accesses display-server
    // state that isn't initialized yet and will deadlock before the main loop runs.
    {
        Win32Message m{};
        m.hwnd = hwnd;

        // WM_NCCREATE (0x81) — signals window created; lParam = 1 (non-zero) = success
        m.message = 0x0081; m.w_param = 0; m.l_param = 1;
        push_message(m);

        // WM_CREATE (0x01)
        m.message = WM_CREATE; m.w_param = 0; m.l_param = 0;
        push_message(m);

        // WM_SIZE — report initial client area
        m.message = WM_SIZE;
        m.w_param = 0;
        m.l_param = static_cast<s64>((static_cast<u64>(w_h) << 16) | static_cast<u16>(w_w));
        push_message(m);

        // WM_ACTIVATE — WA_ACTIVE=1 in low word
        m.message = WM_ACTIVATE; m.w_param = 1; m.l_param = 0;
        push_message(m);

        // WM_SETFOCUS
        m.message = WM_SETFOCUS; m.w_param = 0; m.l_param = 0;
        push_message(m);
    }

    if (!hide) show_window(hwnd, 5 /*SW_SHOW*/);
    return hwnd;
}

void X11WindowManager::destroy_window(void* hwnd) {
    auto* w = window_from_hwnd(hwnd);
    if (!w) return;
    dispatch_message_impl(hwnd, WM_DESTROY, 0, 0);
    if (w->display && w->xid) XDestroyWindow(w->display, static_cast<Window>(w->xid));
    if (w->gc) XFreeGC(w->display, w->gc);
    if (w->fb) { free(w->fb); w->fb = nullptr; w->fb_size = 0; }
    windows_.erase(hwnd);
}

void X11WindowManager::show_window(void* hwnd, int /*cmd_show*/) {
    auto* w = window_from_hwnd(hwnd);
    if (!w || !w->display || !w->xid) return;
    Window xw = static_cast<Window>(w->xid);
    XMapRaised(w->display, xw);

    Atom net_active = XInternAtom(w->display, "_NET_ACTIVE_WINDOW", False);
    XEvent xev{};
    xev.xclient.type = ClientMessage;
    xev.xclient.serial = 0;
    xev.xclient.send_event = True;
    xev.xclient.window = xw;
    xev.xclient.message_type = net_active;
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = 1;
    xev.xclient.data.l[1] = CurrentTime;
    xev.xclient.data.l[2] = 0;
    XSendEvent(w->display, DefaultRootWindow(w->display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &xev);

    XFlush(w->display);
    XSync(w->display, False);
    w->visible = true;

    // Post show/activate/focus to queue — do NOT dispatch synchronously
    Win32Message m{};
    m.hwnd = hwnd;
    m.message = 0x0018 /*WM_SHOWWINDOW*/; m.w_param = 1; m.l_param = 0; push_message(m);
    m.message = WM_ACTIVATE; m.w_param = 1; m.l_param = 0; push_message(m);
    m.message = WM_SETFOCUS; m.w_param = 0; m.l_param = 0; push_message(m);
    // Mark window dirty for first WM_PAINT delivery
    invalidate(hwnd);
}

void X11WindowManager::update_window(void* hwnd) {
    auto* w = window_from_hwnd(hwnd);
    if (!w || !w->display || !w->xid) return;
    XFlush(w->display);
    XSync(w->display, False);
}

// Ensure the window has a software backbuffer of the requested size and return it.
u8* X11WindowManager::surface_buffer(void* hwnd, int w, int h) {
    auto* nw = window_from_hwnd(hwnd);
    if (!nw) return nullptr;
    if (w > 0 && h > 0) { nw->width = w; nw->height = h; }
    u32 need = static_cast<u32>(nw->width) * static_cast<u32>(nw->height) * 4;
    u32 have = (nw->fb) ? nw->fb_size : 0;
    if (!nw->fb) {
        nw->fb = static_cast<u8*>(calloc(need, 1));
        nw->fb_size = need;
    } else if (need > have) {
        nw->fb = static_cast<u8*>(realloc(nw->fb, need));
        if (nw->fb) nw->fb_size = need;
    }
    return nw->fb;
}

// Blit the software RGBA backbuffer into the X11 window.
void X11WindowManager::surface_present(void* hwnd) {
    auto* nw = window_from_hwnd(hwnd);
    if (!nw || !nw->display || !nw->xid || !nw->fb) return;
    int w = nw->width, h = nw->height;
    if (w <= 0 || h <= 0) return;

    XImage* img = XCreateImage(nw->display, DefaultVisual(nw->display, DefaultScreen(nw->display)),
                               24, ZPixmap, 0, nullptr,
                               static_cast<unsigned>(w), static_cast<unsigned>(h), 32, 0);
    if (!img) return;
    // XCreateImage with data=NULL allocates its own buffer (bytes_per_line*height)
    // that XDestroyImage will free. We copy the window backbuffer into it so we
    // never hand our owned nw->fb to X (XDestroyImage would free() it -> the
    // later destroy_window free is a double-free / heap corruption).
    if (img->data) {
        std::memcpy(img->data, nw->fb, static_cast<size_t>(img->bytes_per_line) * static_cast<size_t>(h));
        XPutImage(nw->display, static_cast<Drawable>(nw->xid),
                  nw->gc ? nw->gc : DefaultGC(nw->display, DefaultScreen(nw->display)),
                  img, 0, 0, 0, 0, static_cast<unsigned>(w), static_cast<unsigned>(h));
    }
    XDestroyImage(img);
    XFlush(nw->display);
    nw->fb_dirty = false;
}

void* X11WindowManager::first_window() {
    return windows_.empty() ? nullptr : windows_.begin()->first;
}

std::uint64_t X11WindowManager::xwindow_of(void* hwnd) {
    auto it = windows_.find(hwnd);
    return (it != windows_.end()) ? it->second->xid : 0;
}

std::uint64_t X11WindowManager::get_window_long(void* hwnd, int nIndex) {
    auto it = windows_.find(hwnd);
    if (it == windows_.end()) return 0;
    auto* w = it->second.get();
    switch (nIndex) {
        case -21: return w->userdata;   // GWLP_USERDATA
        case -20: return w->ex_style;   // GWL_EXSTYLE
        case -16: return w->style;      // GWL_STYLE
        case -4:  return reinterpret_cast<std::uint64_t>(w->custom_wndproc ? w->custom_wndproc : (w->cls ? w->cls->window_proc : nullptr)); // GWLP_WNDPROC
        case -6:  return reinterpret_cast<std::uint64_t>(w->cls ? w->cls->instance_handle : nullptr); // GWLP_HINSTANCE
        default:  return 0;
    }
}
void X11WindowManager::set_window_long(void* hwnd, int nIndex, std::uint64_t value) {
    auto it = windows_.find(hwnd);
    if (it == windows_.end()) return;
    auto* w = it->second.get();
    if (nIndex == -21) w->userdata = value;
    else if (nIndex == -20) w->ex_style = static_cast<u32>(value);
    else if (nIndex == -16) w->style = static_cast<u32>(value);
    else if (nIndex == -4) w->custom_wndproc = reinterpret_cast<void*>(value);
}

// ---- Win32 timers --------------------------------------------------------------
static u64 timer_now_ms() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000ULL + static_cast<u64>(ts.tv_nsec) / 1000000ULL;
}
void* X11WindowManager::set_timer(void* hwnd, int id, u32 interval_ms) {
    if (interval_ms == 0) interval_ms = 1;
    std::lock_guard<std::mutex> lk(timers_mutex_);
    // Re-setting an existing (hwnd,id) timer just updates it.
    for (auto& t : timers_) {
        if (t.hwnd == hwnd && t.id == id) {
            t.interval_ms = interval_ms;
            t.next_fire_ms = timer_now_ms() + interval_ms;
            return reinterpret_cast<void*>(static_cast<uintptr_t>(id));
        }
    }
    timers_.push_back({hwnd, id, interval_ms, timer_now_ms() + interval_ms});
    return reinterpret_cast<void*>(static_cast<uintptr_t>(id));
}
bool X11WindowManager::kill_timer(void* hwnd, int id) {
    std::lock_guard<std::mutex> lk(timers_mutex_);
    for (size_t i = 0; i < timers_.size(); ++i) {
        if (timers_[i].hwnd == hwnd && timers_[i].id == id) {
            timers_.erase(timers_.begin() + i);
            return true;
        }
    }
    return false;
}
bool X11WindowManager::poll_timer(Win32Message& out) {
    u64 now = timer_now_ms();
    std::lock_guard<std::mutex> lk(timers_mutex_);
    for (auto& t : timers_) {
        if (now >= t.next_fire_ms) {
            t.next_fire_ms = now + t.interval_ms;   // periodic re-arm
            out.hwnd = t.hwnd; out.message = WM_TIMER;
            out.w_param = static_cast<u64>(t.id); out.l_param = 0;
            return true;
        }
    }
    return false;
}

void X11WindowManager::get_window_rect(void* hwnd, void* lpRect) {
    auto* w = window_from_hwnd(hwnd);
    if (!w || !lpRect) return;
    auto* r = static_cast<s32*>(lpRect);
    // RECT: left, top, right, bottom.
    XWindowAttributes wa{};
    if (w->display && w->xid && XGetWindowAttributes(w->display, static_cast<Window>(w->xid), &wa)) {
        r[0] = wa.x; r[1] = wa.y; r[2] = wa.x + wa.width; r[3] = wa.y + wa.height;
    } else {
        r[0] = w->x; r[1] = w->y; r[2] = w->x + w->width; r[3] = w->y + w->height;
    }
}

void X11WindowManager::get_client_rect(void* hwnd, void* lpRect) {
    auto* w = window_from_hwnd(hwnd);
    if (!w || !lpRect) return;
    auto* r = static_cast<s32*>(lpRect);
    r[0] = 0; r[1] = 0; r[2] = w->width; r[3] = w->height;
}

// ---------------------------------------------------------------------------
// Message pump
// ---------------------------------------------------------------------------
static void XTranslateKey(KeySym ks, bool down, u32& vk) {
    (void)down;
    switch (ks) {
        case XK_Return: vk = 0x0D; break;
        case XK_Escape: vk = 0x1B; break;
        case XK_space:  vk = 0x20; break;
        case XK_BackSpace: vk = 0x08; break;
        case XK_Tab:    vk = 0x09; break;
        case XK_Left:   vk = 0x25; break;
        case XK_Up:     vk = 0x26; break;
        case XK_Right:  vk = 0x27; break;
        case XK_Down:   vk = 0x28; break;
        case XK_Shift_L:
        case XK_Shift_R: vk = 0x10; break;
        case XK_Control_L:
        case XK_Control_R: vk = 0x11; break;
        default:
            if (ks >= XK_a && ks <= XK_z) vk = 0x41 + (ks - XK_a);
            else if (ks >= XK_A && ks <= XK_Z) vk = 0x41 + (ks - XK_A);
            else if (ks >= XK_0 && ks <= XK_9) vk = 0x30 + (ks - XK_0);
            else vk = 0x00;
    }
}

void X11WindowManager::pump_x11_events() {
    if (!display_ || !initialized_) return;
    while (XPending(display_) > 0) {
        XEvent ev;
        XNextEvent(display_, &ev);
        // Find which of our windows owns this event's X window.
        NativeWindow* target = nullptr;
        for (auto& [k, w] : windows_) {
            if (w->xid == static_cast<std::uint64_t>(ev.xany.window)) { target = w.get(); break; }
        }
        if (!target) continue;

        Win32Message msg{};
        msg.hwnd = target->hwnd;
        msg.time = static_cast<u32>(ev.xany.serial & 0xFFFFFFFF);
        msg.pt_x = ev.xany.window ? ev.xbutton.x : 0;
        msg.pt_y = ev.xany.window ? ev.xbutton.y : 0;

        switch (ev.type) {
            case Expose:
                msg.message = WM_PAINT;
                msg.w_param = 0; msg.l_param = 0;
                break;
            case ConfigureNotify:
                target->width  = ev.xconfigure.width;
                target->height = ev.xconfigure.height;
                msg.message = WM_SIZE;
                msg.w_param = 0; msg.l_param = (static_cast<u64>(ev.xconfigure.height) << 32) |
                                              static_cast<u32>(ev.xconfigure.width);
                break;
            case KeyPress:
            case KeyRelease: {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                bool down = (ev.type == KeyPress);
                u32 vk = 0;
                XTranslateKey(ks, down, vk);
                msg.message = down ? WM_KEYDOWN : WM_KEYUP;
                msg.w_param = vk; msg.l_param = 0;
                break;
            }
            case ButtonPress:
            case ButtonRelease: {
                bool down = (ev.type == ButtonPress);
                u32 vk = 0;
                if (ev.xbutton.button == 1) vk = 0x01;      // WM_LBUTTON
                else if (ev.xbutton.button == 3) vk = 0x02; // WM_RBUTTON
                else if (ev.xbutton.button == 2) vk = 0x04; // WM_MBUTTON
                msg.message = down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                if (vk == 0x02) msg.message = down ? 0x0204 : 0x0205; // RBUTTON
                if (vk == 0x04) msg.message = down ? 0x0207 : 0x0208; // MBUTTON
                msg.w_param = vk;
                msg.l_param = (static_cast<u64>(ev.xbutton.y) << 16) |
                              static_cast<u16>(ev.xbutton.x);
                msg.pt_x = ev.xbutton.x; msg.pt_y = ev.xbutton.y;
                break;
            }
            case MotionNotify:
                msg.message = WM_MOUSEMOVE;
                msg.w_param = 0;
                msg.l_param = (static_cast<u64>(ev.xmotion.y) << 16) |
                              static_cast<u16>(ev.xmotion.x);
                msg.pt_x = ev.xmotion.x; msg.pt_y = ev.xmotion.y;
                break;
            case DestroyNotify:
                msg.message = WM_DESTROY;
                break;
            default:
                continue;
        }
        push_message(msg);
    }
}

bool X11WindowManager::pop_message(Win32Message& out) {
    std::lock_guard<std::mutex> lk(q_mutex_);
    if (queue_.empty()) return false;
    out = queue_.front();
    queue_.pop_front();
    return true;
}

void X11WindowManager::invalidate(void* hwnd) {
    auto it = windows_.find(hwnd);
    if (it != windows_.end()) it->second->paint_pending = true;
}

// If a window is marked paint_pending, synthesise one WM_PAINT (and clear the
// flag). hwnd==0 means the first invalid window. Returns true if a message was
// produced.
bool X11WindowManager::synthesize_paint(void* hwnd, Win32Message& out) {
    void* target = hwnd;
    if (!target) for (auto& [h, w] : windows_)
        if (w->paint_pending) { target = h; break; }
    if (!target) return false;
    auto it = windows_.find(target);
    if (it == windows_.end() || !it->second->paint_pending) return false;
    it->second->paint_pending = false;
    out.hwnd = target; out.message = WM_PAINT; out.w_param = 0; out.l_param = 0;
    return true;
}

int X11WindowManager::get_message(void* lpMsg, void* hwnd, u32 min, u32 max) {
    for (;;) {
        if (quit_requested_) {
            std::lock_guard<std::mutex> lk(q_mutex_);
            if (queue_.empty()) {
                if (lpMsg) {
                    auto* m = static_cast<Win32Message*>(lpMsg);
                    m->hwnd = hwnd;
                    m->message = WM_QUIT;
                    m->w_param = static_cast<u64>(exit_code_);
                    m->l_param = 0;
                }
                return 0;
            }
        }
        pump_x11_events();
        {
            std::lock_guard<std::mutex> lk(q_mutex_);
            for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                if (hwnd && it->hwnd != hwnd) continue;
                if (min || max) {
                    if (it->message < min || it->message > max) continue;
                }
                Win32Message m = *it;
                queue_.erase(it);
                if (lpMsg) *static_cast<Win32Message*>(lpMsg) = m;
                return (m.message == WM_QUIT) ? 0 : 1;
            }
        }
        Win32Message m;
        if (synthesize_paint(hwnd, m)) {
            if (lpMsg) *static_cast<Win32Message*>(lpMsg) = m;
            return 1;
        }
        if (poll_timer(m)) {
            if (lpMsg) *static_cast<Win32Message*>(lpMsg) = m;
            return 1;
        }
        if (display_) XFlush(display_);
        struct timespec ts{0, 2'000'000};
        nanosleep(&ts, nullptr);
    }
}

int X11WindowManager::peek_message(void* lpMsg, void* hwnd, u32 min, u32 max, u32 remove) {
    pump_x11_events();
    {
        std::lock_guard<std::mutex> lk(q_mutex_);
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (hwnd && it->hwnd != hwnd) continue;
            if (min || max) {
                if (it->message < min || it->message > max) continue;
            }
            if (lpMsg) *static_cast<Win32Message*>(lpMsg) = *it;
            if (remove & 1) { // PM_REMOVE
                queue_.erase(it);
            }
            return 1;
        }
    }
    Win32Message m;
    if (synthesize_paint(hwnd, m)) {
        if (lpMsg) *static_cast<Win32Message*>(lpMsg) = m;
        return 1;
    }
    if (poll_timer(m)) {
        if (lpMsg) *static_cast<Win32Message*>(lpMsg) = m;
        return 1;
    }
    return 0;
}

int X11WindowManager::translate_message(const void* /*lpMsg*/) { return 1; }

int X11WindowManager::dispatch_message(const void* lpMsg) {
    if (!lpMsg) return 0;
    const auto* m = static_cast<const Win32Message*>(lpMsg);
    return static_cast<int>(dispatch_message_impl(m->hwnd, m->message, m->w_param, m->l_param));
}

// Internal: call the guest WNDPROC or DefWindowProc.
s64 X11WindowManager::dispatch_message_impl(void* hwnd, u32 msg, u64 wparam, s64 lparam) {
    auto* w = window_from_hwnd(hwnd);
    if (!w) return 0;
    void* proc = w->custom_wndproc ? w->custom_wndproc : (w->cls ? w->cls->window_proc : nullptr);
    if (!proc) return 0;
    using WndProc = s64 (__attribute__((ms_abi))*)(void*, u32, u64, s64);
    auto fn = reinterpret_cast<WndProc>(proc);
    try {
        return fn(hwnd, msg, wparam, lparam);
    } catch (...) {
        return 0;
    }
}

void X11WindowManager::post_quit_message(int exit_code) {
    exit_code_ = exit_code;
    quit_requested_ = true;
    Win32Message m{};
    m.message = WM_QUIT;
    m.w_param = static_cast<u64>(exit_code);
    push_message(m);
}

int X11WindowManager::post_message_a(void* hwnd, u32 msg, u64 wparam, s64 lparam) {
    Win32Message m{};
    m.hwnd = hwnd; m.message = msg; m.w_param = wparam; m.l_param = lparam;
    push_message(m);
    return 1;
}

int X11WindowManager::send_message_a(void* hwnd, u32 msg, u64 wparam, s64 lparam) {
    return static_cast<int>(dispatch_message_impl(hwnd, msg, wparam, lparam));
}

// ---------------------------------------------------------------------------
// DefWindowProc default handling
// ---------------------------------------------------------------------------
s64 X11WindowManager::def_window_proc(void* hwnd, u32 msg, u64 wparam, s64 lparam) {
    auto* w = window_from_hwnd(hwnd);
    switch (msg) {
        case WM_CLOSE:
            destroy_window(hwnd);
            return 0;
        case WM_DESTROY:
            post_quit_message(0);
            return 0;
        case WM_SIZE:
        case WM_PAINT:
        case WM_MOUSEMOVE:
            return 0;
        case WM_CREATE:
            return 0;
        case 0x0024: // WM_GETMINMAXINFO
            return 0;
        default:
            (void)w; (void)lparam; (void)wparam;
            return 0;
    }
}

// ---------------------------------------------------------------------------
// DC / GDI
// ---------------------------------------------------------------------------
void* X11WindowManager::get_dc(void* hwnd) {
    auto* w = window_from_hwnd(hwnd);
    return w ? static_cast<void*>(w->gc) : nullptr;
}
int X11WindowManager::release_dc(void* hwnd, void* dc) {
    (void)hwnd; (void)dc; return 1;
}

} // namespace papaya::win32