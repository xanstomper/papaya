#pragma once

#include "papaya/common/types.hpp"

// Win32 registry emulation for the native HLE.
//
// Games read install paths, graphics settings, and config keys from the
// registry at startup. This provides a small, real registry: an in-memory key
// tree rooted at the standard predefined handles (HKCR/HKCU/HKLM), persisted to
// a JSON file so values set by one run survive. RegOpenKeyExA/W returns a
// handle; RegQueryValueExA/W returns values. Simple and real, no mocks.
//
// Handle model: handles are small integers (HKEY tree nodes). Predefined roots:
// 0x80000000=HKCR, 0x80000001=HKCU, 0x80000002=HKLM, 0x80000003=HKUSERS.
namespace papaya::win32 {

// Opens (creating if needed) a subkey under a root/subkey path. Returns a handle
// >= 0x100 on success (ERROR_SUCCESS=0), a negative error code otherwise.
// path uses '\\' separators; empty path opens the root itself.
s32 registry_open_key(u32 root_handle, const char* path, bool create, void** out_key);
s32 registry_close_key(void* key);
// Set/query a REG_SZ / REG_DWORD value. type 1=REG_SZ, 4=REG_DWORD.
s32 registry_set_value(void* key, const char* name, u32 type, const void* data, u32 cb);
s32 registry_query_value(void* key, const char* name, u32* type_out, void* data, u32* cb_inout);
s32 registry_get_value(void* key, const char* name, u32* type_out, void* data, u32* cb_inout);
s32 registry_delete_value(void* key, const char* name);
// Populate a few standard keys games commonly probe.
void registry_seed();

} // namespace papaya::win32