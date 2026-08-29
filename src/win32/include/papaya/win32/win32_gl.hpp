#pragma once

#include "papaya/common/types.hpp"

// OpenGL (wgl) context for the native Win32 HLE.
//
// wglCreateContext/MakeCurrent/DeleteContext back a real GLX context on the
// window manager's X display, so guest OpenGL calls (routed through Mesa via
// opengl32.dll / wglGetProcAddress) render into the native window. This is the
// tractable real-3D path (Mesa llvmpipe / Intel GL), unlike D3D which is
// DXVK-class.
namespace papaya::win32 {

typedef void* HWGL;

HWGL wgl_create_context(void* hdc);        // returns GLXContext* or null
int   wgl_make_current(void* hdc, HWGL hglrc);
int   wgl_delete_context(HWGL hglrc);

} // namespace papaya::win32