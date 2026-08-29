#include "papaya/win32/win32_dsound.hpp"
#include "papaya/common/logger.hpp"
#include <cstring>
#include <cstdlib>

// Reference vtable layouts (DirectSound COM, ms_abi), indices from mingw-w64
// dsound.h:
//   IDirectSound:   0-2 IUnknown, 3 CreateSoundBuffer, 4 GetCaps,
//                   5 DuplicateSoundBuffer, 6 SetCooperativeLevel, ...
//   IDirectSoundBuffer: 0-2 IUnknown, 3 GetCaps, 4 GetCurPos, 5 GetFormat,
//                   6 GetVolume, ... 11 Lock, 12 Play, 13 SetCurPos, 14 SetFormat,
//                   15 SetFrequency, 16 SetPan, 17 SetVolume, 18 Stop, 19 Unlock.
namespace papaya::win32 {

#define D3DMS __attribute__((ms_abi))

namespace {
struct DsObject    { void** vtbl; };
struct DirectSound : DsObject {};
struct SoundBuffer : DsObject {
    u32 state{0};   // 0 stopped, 1 playing
    long volume{0}; // hundredths of dB (0 = full)
};

// ---- IUnknown ---------------------------------------------------------------
static D3DMS u64 ds_qi(void* self, u64, void* ppv) { if (ppv) *reinterpret_cast<void**>(ppv)=self; return 0; }
static D3DMS u64 ds_addref(void*) { return 1; }
static D3DMS u64 ds_rel(void*) { return 0; }

// ---- IDirectSoundBuffer method thunks ---------------------------------------
static D3DMS u64 buf_set_volume(void* self, long vol) {
    auto* b = static_cast<SoundBuffer*>(self);
    if (b) b->volume = vol;
    return 0;
}
static D3DMS u64 buf_get_volume(void* self) {
    auto* b = static_cast<SoundBuffer*>(self);
    return b ? (u64)b->volume : 0;
}
static D3DMS u64 buf_stop(void* self) {
    if (self) static_cast<SoundBuffer*>(self)->state = 0;
    return 0;
}
static D3DMS u64 buf_play(void* self, u64, u64, u64) {
    if (self) static_cast<SoundBuffer*>(self)->state = 1;
    return 0; // DS_OK
}
static D3DMS u64 buf_release(void* self) {
    auto* b = static_cast<SoundBuffer*>(self);
    if (b) { free(b->vtbl); delete b; }
    return 0;
}

// ---- IDirectSound -----------------------------------------------------------
static D3DMS u64 ds_create_sound_buffer(void* self, void* desc, void* buf_out, void* unk) {
    (void)self; (void)unk;
    auto* b = new SoundBuffer();
    b->vtbl = static_cast<void**>(calloc(32, sizeof(void*)));
    for (int i=0;i<32;i++) b->vtbl[i]=reinterpret_cast<void*>(&ds_rel); // safe no-ops
    b->vtbl[0]=reinterpret_cast<void*>(&ds_qi);
    b->vtbl[1]=reinterpret_cast<void*>(&ds_addref);
    b->vtbl[2]=reinterpret_cast<void*>(&buf_release);
    b->vtbl[6]=reinterpret_cast<void*>(&buf_get_volume);
    b->vtbl[11]=reinterpret_cast<void*>(&ds_rel);  // Lock
    b->vtbl[12]=reinterpret_cast<void*>(&buf_play);
    b->vtbl[17]=reinterpret_cast<void*>(&buf_set_volume);
    b->vtbl[18]=reinterpret_cast<void*>(&buf_stop);
    b->vtbl[19]=reinterpret_cast<void*>(&ds_rel);  // Unlock
    (void)desc;
    if (buf_out) *reinterpret_cast<void**>(buf_out) = b;
    return 0; // DS_OK
}
static D3DMS u64 ds_set_cooperative_level(void*, void*, u64) { return 0; }
} // namespace

int dsound_create8(const void* guid, void** ods8_out) {
    (void)guid;
    auto* ds = new DirectSound();
    ds->vtbl = static_cast<void**>(calloc(16, sizeof(void*)));
    for (int i=0;i<16;i++) ds->vtbl[i]=reinterpret_cast<void*>(&ds_rel);
    ds->vtbl[0]=reinterpret_cast<void*>(&ds_qi);
    ds->vtbl[1]=reinterpret_cast<void*>(&ds_addref);
    ds->vtbl[2]=reinterpret_cast<void*>(&ds_rel);
    ds->vtbl[3]=reinterpret_cast<void*>(&ds_create_sound_buffer);
    ds->vtbl[6]=reinterpret_cast<void*>(&ds_set_cooperative_level);
    if (ods8_out) *ods8_out = (void*)ds;
    return 0; // DS_OK
}

int dsound_set_cooperative_level(void* ds, void* hwnd, u32 level) {
    return (int)((D3DMS u64(*)(void*,void*,u64))(((void**)ds)[6]))(ds, hwnd, level);
}
int dsound_create_sound_buffer(void* ds, const void* desc, void** buffer_out, void* unk) {
    return (int)((D3DMS u64(*)(void*,const void*,void**,void*))(((void**)ds)[3]))(ds, desc, buffer_out, unk);
}
int dsound_buffer_play(void* buffer, u32 r1, u32 r2, u32 flags) {
    return (int)((D3DMS u64(*)(void*,u64,u64,u64))(((void**)buffer)[12]))(buffer,r1,r2,flags);
}
int dsound_buffer_stop(void* buffer) {
    return (int)((D3DMS u64(*)(void*))(((void**)buffer)[18]))(buffer);
}
int dsound_buffer_set_volume(void* buffer, long vol) {
    return (int)((D3DMS u64(*)(void*,long))(((void**)buffer)[17]))(buffer,vol);
}
long dsound_buffer_get_volume(void* buffer) {
    return (long)((D3DMS u64(*)(void*))(((void**)buffer)[6]))(buffer);
}
u32 dsound_buffer_release(void* buffer) {
    return (u32)((D3DMS u64(*)(void*))(((void**)buffer)[2]))(buffer);
}

} // namespace papaya::win32