#include "papaya/win32/win32_gl.hpp"
#include "papaya/win32/win32_window.hpp"
#include "papaya/common/logger.hpp"
#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <dlfcn.h>
#include <cstdlib>

// Real GLX-backed wgl contexts. The guest's opengl32.dll gl* imports and
// wglGetProcAddress calls resolve to host Mesa GL (dlopen'd libGL.so.1).

namespace papaya::win32 {

namespace {

// A wgl context wraps a native GLXContext plus the display it belongs to.
struct WglContext {
    Display*  dpy;
    GLXContext glx;
    // share-list parent (compatibility profile)
};

Display* wm_display() {
    Display* d = static_cast<Display*>(window_manager().display());
    // The WM display may be null if no window manager was initialized; open our
    // own default display so GLX contexts still work for headless-ish guests.
    if (!d) d = XOpenDisplay(nullptr);
    return d;
}

} // namespace

HWGL wgl_create_context(void* hdc) {
    (void)hdc;
    Display* dpy = wm_display();
    if (!dpy) { log::warn("GL", "wglCreateContext: no X display"); return nullptr; }
    static int att[] = { GLX_RGBA, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, None };
    XVisualInfo* vi = glXChooseVisual(dpy, DefaultScreen(dpy), att);
    if (!vi) { log::warn("GL", "wglCreateContext: no GLX visual"); return nullptr; }
    GLXContext glx = glXCreateContext(dpy, vi, nullptr, GL_TRUE);
    if (!glx) { log::warn("GL", "wglCreateContext: glXCreateContext failed"); return nullptr; }
    auto* c = new WglContext{ dpy, glx };
    return reinterpret_cast<HWGL>(c);
}

int wgl_make_current(void* hdc, HWGL hglrc) {
    // hdc is a GDI DC (GdiDc*); its window_backed flag + associated X window can
    // be surfaced via the window manager, but for correctness we bind to the
    // root/current window. The guest mostly calls MakeCurrent once per swap.
    auto* c = hglrc ? reinterpret_cast<WglContext*>(hglrc) : nullptr;
    if (!c) return 0;   // NULL context = release
    (void)hdc;
    // Find the guest's frontmost window to bind the GL drawable. Use the WM's
    // first window (a NativeWindow* has an .xlib window). We surface an X window
    // via the window manager below.
    Window xwin = 0;
    void* hw = window_manager().first_window();
    if (!hw) {
        hw = window_manager().create_window_ex("PapayaGame", "Papaya Game", 0x10CF0000, 0, 0, 1280, 720, nullptr, nullptr, nullptr, false);
    }
    if (hw) xwin = window_manager().xwindow_of(hw);
    if (!xwin) xwin = DefaultRootWindow(c->dpy);
    Bool ok = glXMakeCurrent(c->dpy, xwin, c->glx);
    return ok ? 1 : 0;
}

int wgl_delete_context(HWGL hglrc) {
    auto* c = hglrc ? reinterpret_cast<WglContext*>(hglrc) : nullptr;
    if (!c) return 1;
    if (c->dpy) glXDestroyContext(c->dpy, c->glx);
    delete c;
    return 1;
}

} // namespace papaya::win32