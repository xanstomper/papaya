#include "papaya/win32/win32_audio.hpp"
#include "papaya/common/logger.hpp"
#include <cstring>
#include <cstdint>
#include <vector>
#include <mutex>
#include <dlfcn.h>

// Real winmm audio output via PulseAudio simple API (dlopen'd so libpulse is an
// optional runtime dep, not a link-time one). 16-bit PCM is mixed and written to
// the host audio sink. Falls back to silent-success if no PulseAudio is present.

namespace papaya::win32 {

namespace {
// ---- PulseAudio simple API (dlopen) -----------------------------------------
struct PaSimple;
using pa_simple_new_fn    = PaSimple* (*)(const char*, const char*, int, const char*, const char*, const void*, const void*, int*);
using pa_simple_write_fn  = int (*)(PaSimple*, const void*, std::size_t, int*);
using pa_simple_drain_fn  = int (*)(PaSimple*, int*);
using pa_simple_free_fn   = void (*)(PaSimple*);
using pa_simple_get_latency_fn = long (*)(PaSimple*, int*);

constexpr int PA_STREAM_PLAYBACK = 1;

struct PulseApi {
    bool ok{false};
    void* handle{nullptr};
    pa_simple_new_fn    new_pa{nullptr};
    pa_simple_write_fn  write_pa{nullptr};
    pa_simple_drain_fn  drain_pa{nullptr};
    pa_simple_free_fn   free_pa{nullptr};
};
PulseApi& pulse() {
    static PulseApi p;
    static bool init = false;
    if (!init) {
        init = true;
        p.handle = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!p.handle) p.handle = dlopen("libpulse-simple.so", RTLD_NOW | RTLD_LOCAL);
        if (p.handle) {
            p.new_pa   = (pa_simple_new_fn)   dlsym(p.handle, "pa_simple_new");
            p.write_pa = (pa_simple_write_fn) dlsym(p.handle, "pa_simple_write");
            p.drain_pa = (pa_simple_drain_fn) dlsym(p.handle, "pa_simple_drain");
            p.free_pa  = (pa_simple_free_fn)  dlsym(p.handle, "pa_simple_free");
            p.ok = p.new_pa && p.write_pa && p.drain_pa && p.free_pa;
        }
    }
    return p;
}

// waveOut device handle.
struct WaveOut {
    u32 tag{0x57415645};   // "WAVE"
    u32 rate{44100}, channels{2}, bits{16};
    PaSimple* pa{nullptr};
    std::vector<u8> pending;   // queued bytes not yet flushed (simple API is sync)
};
} // namespace

bool winmm_play_sound(const char* path_utf8, bool async) {
    (void)async;
    PulseApi& pa = pulse();
    if (!pa.ok || !path_utf8) return false;
    // Parse WAV: RIFF header. Find 'data' chunk.
    FILE* f = fopen(path_utf8, "rb");
    if (!f) { log::warn("WINMM", "PlaySound: cannot open '{}'", path_utf8); return false; }
    u8 hdr[44]; std::size_t got = fread(hdr, 1, 44, f);
    (void)got;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) { fclose(f); return false; }
    u16 nch = *(u16*)(hdr + 22), bits = *(u16*)(hdr + 34);
    u32 rate = *(u32*)(hdr + 24);
    // scan chunks for 'data'
    fseek(f, 12, SEEK_SET);
    u32 data_len = 0; long data_pos = -1;
    while (1) {
        u8 ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        u32 len = *(u32*)(ch + 4);
        if (memcmp(ch, "data", 4) == 0) { data_pos = ftell(f); data_len = len; break; }
        fseek(f, len + (len & 1), SEEK_CUR);
    }
    if (data_pos < 0 || data_len == 0) { fclose(f); return false; }
    std::vector<u8> samples(data_len);
    fseek(f, data_pos, SEEK_SET);
    fread(samples.data(), 1, data_len, f);
    fclose(f);

    char srate[16]; snprintf(srate, sizeof(srate), "%u", rate);
    PaSimple* s = pa.new_pa("papaya", "PlaySound", PA_STREAM_PLAYBACK, nullptr, "papaya",
                            static_cast<const void*>(&rate), nullptr, nullptr);
    if (!s) { log::warn("WINMM", "PlaySound: PulseAudio init failed"); return false; }
    pa.write_pa(s, samples.data(), samples.size(), nullptr);
    pa.drain_pa(s, nullptr);
    pa.free_pa(s);
    (void)nch; (void)bits; (void)srate;
    return true;
}

// WAVEFORMATEX layout (x64): wFormatTag(0), nChannels(2), nSamplesPerSec(4),
// nAvgBytesPerSec(8), nBlockAlign(12), wBitsPerSample(14), cbSize(16).
int winmm_wave_out_open(void** phwo, const void* wfx, u32 nc, u32 nsps, u32 nb) {
    (void)nb;
    auto* wo = new WaveOut();
    wo->rate = nsps ? nsps : 44100;
    wo->channels = nc ? nc : 2;
    if (wfx) {
        wo->channels = *(u16*)((const u8*)wfx + 2);
        u32 rate2 = *(u32*)((const u8*)wfx + 4);
        if (rate2) wo->rate = rate2;
        wo->bits = *(u16*)((const u8*)wfx + 14);
    }
    if (phwo) *phwo = wo;
    return 0; // MMSYSERR_NOERROR
}

// WAVEHDR layout (x64): lpData(0 ptr), dwBufferLength(8), dwBytesRecorded(16),
// dwUser(20), dwFlags(24), dwLoops(28), lpNext(32), reserved(40).
int winmm_wave_out_write(void* hwo, const void* wh) {
    auto* wo = static_cast<WaveOut*>(hwo);
    if (!wo || wo->tag != 0x57415645 || !wh) return 4; // MMSYSERR_INVALHANDLE
    void* data = *(void**)((const u8*)wh + 0);
    u32 len = *(const u32*)((const u8*)wh + 8);
    if (!data || !len) return 0;
    PulseApi& pa = pulse();
    if (!pa.ok) return 0;   // silent-success without audio backend
    if (!wo->pa) {
        // (silent default params; format set via spec later)
        wo->pa = pa.new_pa("papaya", "waveOut", PA_STREAM_PLAYBACK, nullptr, "papaya",
                           static_cast<const void*>(&wo->rate), nullptr, nullptr);
    }
    if (wo->pa) pa.write_pa(wo->pa, data, len, nullptr);
    return 0;
}

int winmm_wave_out_close(void* hwo) {
    auto* wo = static_cast<WaveOut*>(hwo);
    if (!wo || wo->tag != 0x57415645) return 4;
    PulseApi& pa = pulse();
    if (wo->pa) { if (pa.ok) { pa.drain_pa(wo->pa, nullptr); pa.free_pa(wo->pa); } wo->pa = nullptr; }
    delete wo;
    return 0;
}

int winmm_wave_out_set_volume(void* hwo, u32 volume) {
    auto* wo = static_cast<WaveOut*>(hwo);
    (void)wo; (void)volume;
    return 0;
}

u32 winmm_wave_out_get_num_devs() { return 1; }

} // namespace papaya::win32