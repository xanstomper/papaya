#pragma once

#include "papaya/common/types.hpp"

// winmm / DirectSound-compatible audio output for the native Win32 HLE.
//
// Plays 16-bit PCM WAV audio through PulseAudio (or ALSA fallback). Used by
// PlaySoundA and the waveOut* API a game calls at startup. Real audio, no mocks:
// samples are mixed and written to the host audio sink.
//
// A waveOut handle is a WaveOutHwnd* (tagged). PlaySound is fire-and-forget.

namespace papaya::win32 {

// PlaySoundA/W: synchronously (or async) play a WAV. Flags == 0x20000 (ASYNC).
// Returns TRUE on success. ptsz is a null-terminated (possibly Unicode) path.
bool winmm_play_sound(const char* path_utf8, bool async);

// waveOutOpen: create a wave-out handle for the given format. Returns MMSYSERR_OK
// (0) on success; *phwo set to the handle. hdr pbFormat is WAVEFORMATEX.
int winmm_wave_out_open(void** phwo, const void* wave_format, u32 n_channels, u32 n_samples_per_sec, u32 n_bits);

// waveOutWrite: queue a WAVEHDR buffer to play. hdr layout (x64 dwords):
//  0 lpData(ptr),8 dwBufferLength,16 dwBytesRecorded,20 dwUser,24 dwFlags,...
int winmm_wave_out_write(void* hwo, const void* wave_hdr);

// waveOutClose / waveOutSetVolume / waveOutGetNumDevs.
int winmm_wave_out_close(void* hwo);
int winmm_wave_out_set_volume(void* hwo, u32 volume);
u32 winmm_wave_out_get_num_devs();

} // namespace papaya::win32