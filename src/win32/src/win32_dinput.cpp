#include "papaya/win32/win32_dinput.hpp"
#include "papaya/win32/win32_window.hpp"
#include "papaya/common/logger.hpp"
#include <cstring>
#include <cstdlib>
#include <X11/Xlib.h>
#include <X11/keysym.h>

// VK_* constants (subset used for keyboard state)
#ifndef VK_RETURN
#define VK_RETURN 0x0D
#endif
#ifndef VK_SPACE
#define VK_SPACE 0x20
#endif
#ifndef VK_LSHIFT
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_ESCAPE 0x1B
#define VK_TAB 0x09
#define VK_LEFT 0x25
#define VK_RIGHT 0x27
#define VK_UP 0x26
#define VK_DOWN 0x28
#endif

// DirectInput8 COM (ms_abi). A keyboard device's GetDeviceState fills the
// guest's 256-byte DIKEYBOARDSTATE buffer from the X11 keymap.

namespace papaya::win32 {

#define DMS __attribute__((ms_abi))

namespace {
struct Di8        { void** vtbl; };
struct DiDevice8  { void** vtbl; u32 type; }; // 1=keyboard, 2=mouse

static DMS u64 di_qi(void* self, u64, void* ppv) { if (ppv)*reinterpret_cast<void**>(ppv)=self; return 0; }
static DMS u64 di_addref(void*) { return 1; }
static DMS u64 di_rel(void*) { return 0; }

static DMS u64 dev_acquire(void*) { return 0; }     // DI_OK
static DMS u64 dev_unacquire(void*) { return 0; }   // DI_OK
static DMS u64 dev_set_data_format(void*, void*) { return 0; } // DI_OK (accept any format)
static DMS u64 dev_set_cooperative_level(void*, void*, u64) { return 0; }

// GetDeviceState(cbData, lpvData): fill from X keyboard state.
static DMS u64 dev_get_device_state(void* self, u64 cb, void* out) {
    auto* d = static_cast<DiDevice8*>(self);
    if (!d || !out) return 0x80070057;   // E_INVALIDARG
    // Keyboard: fill a 256-byte DIKEYBOARDSTATE with Windows VK codes.
    if (d->type == 1 && cb >= 256) {
        memset(out, 0, 256);
        auto* wm = &window_manager();
        Display* dpy = wm->display();
        char keys[32];
        if (dpy && XQueryKeymap(dpy, keys)) {
            // Translate host X keycodes -> Windows VK by scanning for the
            // specific keycodes we can resolve via XKeysymToKeycode.
            const int vk_keys[] = {'A','B','C','D','E','F','G','H','I','J','K','L','M',
                'N','O','P','Q','R','S','T','U','V','W','X','Y','Z','1','2','3','4','5',
                '6','7','8','9','0', VK_RETURN,VK_SPACE,VK_LSHIFT,VK_RSHIFT,VK_LCONTROL,
                VK_RCONTROL,VK_LMENU,VK_RMENU,VK_ESCAPE,VK_TAB, VK_LEFT,VK_RIGHT,VK_UP,VK_DOWN};
            static const KeySym syms[] = {
                XK_a,XK_b,XK_c,XK_d,XK_e,XK_f,XK_g,XK_h,XK_i,XK_j,XK_k,XK_l,XK_m,
                XK_n,XK_o,XK_p,XK_q,XK_r,XK_s,XK_t,XK_u,XK_v,XK_w,XK_x,XK_y,XK_z,
                XK_1,XK_2,XK_3,XK_4,XK_5,XK_6,XK_7,XK_8,XK_9,XK_0,
                XK_Return,XK_space,XK_Shift_L,XK_Shift_R,XK_Control_L,XK_Control_R,
                XK_Alt_L,XK_Alt_R,XK_Escape,XK_Tab, XK_Left,XK_Right,XK_Up,XK_Down};
            static_assert(sizeof(vk_keys)/sizeof(*vk_keys) == sizeof(syms)/sizeof(*syms));
            for (size_t i = 0; i < sizeof(syms)/sizeof(*syms); ++i) {
                KeyCode kc = (KeyCode)XKeysymToKeycode(dpy, syms[i]);
                if (kc && keys[kc >> 3] & (1 << (kc & 7))) {
                    ((u8*)out)[(u8)vk_keys[i]] = 0x80;
                }
            }
        }
        return 0; // DI_OK
    }
    // Mouse/other: fill a small struct with zero state (no mouse synthesis).
    if (out && cb) memset(out, 0, cb);
    return 0;
}

static DMS u64 dev_get_device_data(void* self, u64 cb, void* out, u32* inout, u64 flags) {
    (void)self;(void)cb;(void)out;(void)flags;
    if (inout && *inout > 0) *inout = 0; // no buffered events
    return 1; // DI_BUFFEROVERFLOW-ish empty is fine -> return DI_OK(0)? Use 0.
}
static DMS u64 dev_get_device_info(void* self, void* info) {
    (void)self; (void)info;
    return 0;
}

// CreateDevice returns a DI8 device (keyboard for GUID_SysKeyboard, else mouse).
static DMS u64 di8_create_device(void* self, void* guid, void** dev_out, void* unk) {
    (void)self;(void)unk;
    auto* d = new DiDevice8();
    d->type = 1; // default keyboard
    // GUID_SysKeyboard = {6f1d2b61-d5a0-11cf-bfc7-444553540000}; mouse = ...d62...
    const unsigned char* g = static_cast<const unsigned char*>(guid);
    if (g && g[0]==0x62 && g[1]==0x2b && g[2]==0x1d && g[3]==0x6f) d->type = 2; // sysmouse
    d->vtbl = static_cast<void**>(calloc(20, sizeof(void*)));
    for (int i=4;i<20;i++) d->vtbl[i]=reinterpret_cast<void*>(&di_rel); // safe no-ops
    d->vtbl[0]=reinterpret_cast<void*>(&di_qi);
    d->vtbl[1]=reinterpret_cast<void*>(&di_addref);
    d->vtbl[2]=reinterpret_cast<void*>(&di_rel);
    d->vtbl[7]=reinterpret_cast<void*>(&dev_acquire);        // Acquire
    d->vtbl[8]=reinterpret_cast<void*>(&dev_unacquire);      // Unacquire
    d->vtbl[9]=reinterpret_cast<void*>(&dev_get_device_state); // GetDeviceState
    d->vtbl[10]=reinterpret_cast<void*>(&dev_get_device_data); // GetDeviceData
    d->vtbl[11]=reinterpret_cast<void*>(&dev_set_data_format); // SetDataFormat
    d->vtbl[13]=reinterpret_cast<void*>(&dev_set_cooperative_level); // SetCooperativeLevel
    if (dev_out) *dev_out = (void*)d;
    return 0; // DI_OK
}

struct Di8Free {
    void* vtbl;
    Di8*  di;
    Di8Free(void* v,Di8* d):vtbl(v),di(d){}
};
// global active Di8 to avoid lifetime of the x display ref issues not needed here
} // namespace
int dinput8_create(void* hinst, u32 version, const void* iid, void** di8_out, void* unk_outer) {
    (void)hinst; (void)version; (void)iid; (void)unk_outer;
    auto* d = new Di8();
    d->vtbl = static_cast<void**>(calloc(12, sizeof(void*)));
    for (int i=0;i<12;i++) d->vtbl[i]=reinterpret_cast<void*>(&di_rel);
    d->vtbl[0]=reinterpret_cast<void*>(&di_qi);
    d->vtbl[1]=reinterpret_cast<void*>(&di_addref);
    d->vtbl[2]=reinterpret_cast<void*>(&di_rel);
    d->vtbl[3]=reinterpret_cast<void*>(&di8_create_device);   // CreateDevice
    if (di8_out) *di8_out = (void*)d;
    return 0; // DI_OK
}

} // namespace papaya::win32