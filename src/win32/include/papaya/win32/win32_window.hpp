#pragma once

#include "papaya/common/types.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <deque>
#include <memory>
#include <mutex>

// Forward-declare Xlib opaque types to avoid leaking X headers into the HLE.
struct _XDisplay;        // Display
struct _XGC;             // GC

namespace papaya::win32 {

// Win32 message (MSG) — layout matches the guest's expectation for the 8-field
// struct: HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time;
// POINT pt. We keep the canonical fields the guest reads.
struct Win32Message {
    void* hwnd{nullptr};
    u32   message{0};
    u64   w_param{0};
    s64   l_param{0};
    u32   time{0};
    s32   pt_x{0};
    s32   pt_y{0};
};

// Common Win32 messages the window layer understands.
enum : u32 {
    WM_NULL        = 0x0000,
    WM_CREATE      = 0x0001,
    WM_DESTROY     = 0x0002,
    WM_SIZE        = 0x0005,
    WM_ACTIVATE    = 0x0006,
    WM_SETFOCUS    = 0x0007,
    WM_KILLFOCUS   = 0x0008,
    WM_PAINT       = 0x000F,
    WM_CLOSE       = 0x0010,
    WM_QUIT        = 0x0012,
    WM_KEYDOWN     = 0x0100,
    WM_KEYUP       = 0x0101,
    WM_LBUTTONDOWN = 0x0201,
    WM_LBUTTONUP   = 0x0202,
    WM_MOUSEMOVE   = 0x0200,
};

// A registered window class.
struct NativeWindowClass {
    std::string  name;
    void*        window_proc{nullptr};   // guest WNDPROC
    void*        instance_handle{nullptr};
    bool         registered{false};
};

// A live native window (one HWND == one of these).
struct NativeWindow {
    void*              hwnd{nullptr};     // self pointer used as HWND
    NativeWindowClass* cls{nullptr};
    std::string        title;
    _XDisplay*         display{nullptr};
    std::uint64_t      xid{0};            // X11 Window id
    _XGC*              gc{nullptr};
    int                x{0}, y{0}, width{0}, height{0};
    bool               visible{false};
    u32                style{0};
};

// X11-backed Win32 window manager. Owns the display + per-window state + the
// Win32 message queue. HWNDs are NativeWindow*; classes are NativeWindowClass*.
class X11WindowManager {
public:
    X11WindowManager();
    ~X11WindowManager();

    // Lifecycle
    bool initialize();
    void shutdown();

    // Window classes
    void* register_class(const char* name, void* wndproc, void* hinstance);
    void* find_class(const char* name);

    // Window creation
    void* create_window_ex(const char* class_name, const char* title, u32 style,
                           int x, int y, int w, int h, void* parent, void* instance,
                           void* param, bool hide);

    void  destroy_window(void* hwnd);
    void  show_window(void* hwnd, int cmd_show);
    void  update_window(void* hwnd);
    void  get_window_rect(void* hwnd, void* lpRect);
    void  get_client_rect(void* hwnd, void* lpRect);

    // Message pump
    int   get_message(void* lpMsg, void* hwnd, u32 min, u32 max);
    int   peek_message(void* lpMsg, void* hwnd, u32 min, u32 max, u32 remove);
    int   translate_message(const void* lpMsg);
    int   dispatch_message(const void* lpMsg);
    void  post_quit_message(int exit_code);
    int   post_message_a(void* hwnd, u32 msg, u64 wparam, s64 lparam);
    int   send_message_a(void* hwnd, u32 msg, u64 wparam, s64 lparam);

    // DefWindowProc default handling
    s64   def_window_proc(void* hwnd, u32 msg, u64 wparam, s64 lparam);

    // DC / GDI helpers (minimal, bitmap-less for now)
    void* get_dc(void* hwnd);
    int   release_dc(void* hwnd, void* dc);

    // Accumulate an X11 event into the guest queue (translates to a MSG).
    void pump_x11_events();

    int  get_exit_code() const { return exit_code_; }

private:
    NativeWindow* window_from_hwnd(void* hwnd) {
        auto it = windows_.find(hwnd);
        return it == windows_.end() ? nullptr : it->second.get();
    }
    void push_message(const Win32Message& msg) { {
        std::lock_guard<std::mutex> lk(q_mutex_);
        queue_.push_back(msg);
    } }
    bool pop_message(Win32Message& out);
    // Directly invoke the guest WNDPROC for a message (no queue).
    s64 dispatch_message_impl(void* hwnd, u32 msg, u64 wparam, s64 lparam);

    _XDisplay*   display_{nullptr};
    bool         initialized_{false};
    bool         quit_requested_{false};
    int          exit_code_{0};

    std::unordered_map<std::string, NativeWindowClass> classes_;
    std::unordered_map<void*, std::unique_ptr<NativeWindow>> windows_;
    std::deque<Win32Message> queue_;
    std::mutex               q_mutex_;
};

// Global manager (owned by the HLE for now, single instance).
X11WindowManager& window_manager();

} // namespace papaya::win32