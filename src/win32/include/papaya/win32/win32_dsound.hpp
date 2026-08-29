#pragma once

#include "papaya/common/types.hpp"

// DirectSound8 COM-style audio for the native Win32 HLE.
//
// DirectSoundCreate8 returns an IDirectSound8 object (ms_abi vtable). The
// realistic subset games use: SetCooperativeLevel, CreateSoundBuffer,
// IDirectSoundBuffer8::Play/Stop/SetVolume/Release, and a primary-buffer lock.
// Audio is routed to the same PulseAudio path as winmm (win32_audio), so a
// sound buffer's PCM plays through the host audio sink. No mocks.
namespace papaya::win32 {

// DirectSoundCreate8 -> *ods8 = IDirectSound8 object. Returns DS_OK (0).
int dsound_create8(const void* guid, void** ods8_out);

// IDirectSound8::SetCooperativeLevel — accept hwnd + flag, no-op (audio runs).
int dsound_set_cooperative_level(void* ds, void* hwnd, u32 level);

// IDirectSound8::CreateSoundBuffer(desc, &buffer, unk) — create a playable
// sound-buffer object. desc is DSBUFFERDESC: dwSize(0), dwFlags(4), ...
// dwBufferBytes at offset 12; the buffer plays silence/queued PCM.
int dsound_create_sound_buffer(void* ds, const void* desc, void** buffer_out, void* unk);

// IDirectSoundBuffer8::Play(dwReserved1, dwReserved2, dwFlags)
int dsound_buffer_play(void* buffer, u32 r1, u32 r2, u32 flags);
// IDirectSoundBuffer8::Stop
int dsound_buffer_stop(void* buffer);
// IDirectSoundBuffer8::SetVolume / GetVolume
int dsound_buffer_set_volume(void* buffer, long volume);
long dsound_buffer_get_volume(void* buffer);
// IDirectSoundBuffer8::Release (IUnknown)
u32 dsound_buffer_release(void* buffer);

} // namespace papaya::win32