#pragma once

#include "papaya/common/types.hpp"

namespace papaya::win32 {

// WASAPI (mmdevapi) minimal COM surface backed by PulseAudio. Returns true if
// rclsid/riid were handled (CLSID_MMDeviceEnumerator / IID_IMMDeviceEnumerator)
// and *ppv was set. Hooked from Win32ApiHle::hle_co_create_instance.
bool mmdevice_try_create(const void* rclsid, const void* riid, void** ppv);
// true if p is one of our static COM-owned buffers (CoTaskMemFree no-op).
bool mmdevice_is_static_ptr(const void* p);

} // namespace papaya::win32
