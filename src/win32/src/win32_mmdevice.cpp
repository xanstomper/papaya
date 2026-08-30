#include "papaya/win32/win32_audio.hpp"
#include "papaya/win32/win32_api_hle.hpp"
#include "papaya/win32/win32_mmdevice.hpp"
#include "papaya/common/logger.hpp"
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <vector>
#include <dlfcn.h>
#include <unistd.h>

// Minimal but real WASAPI (mmdevapi) emulation: IMMDeviceEnumerator +
// IMMDevice + IAudioClient + IAudioRenderClient COM objects backed by a
// PulseAudio simple stream, so Godot's WASAPI driver gets actual audio data.
// Guest audio flow:
//   CoCreateInstance(CLSID_MMDeviceEnumerator) -> GetDefaultAudioEndpoint
//   -> Activate(IAudioClient) -> GetMixFormat -> Initialize(shared,eventcallback)
//   -> GetBufferSize -> SetEventHandle -> GetService(IAudioRenderClient)
//   -> thread: WaitForSingleObject(ev,100) -> GetBuffer -> CopyTo -> ReleaseBuffer
// We expose a ring buffer; the game fills it via GetBuffer/ReleaseBuffer and a
// dedicated writer thread drains it into PulseAudio (silent-drop if no PA).

namespace papaya::win32 {

#define D3DMS __attribute__((ms_abi))

namespace {

// ---- CLSID/IID (as little-endian GUID byte arrays) -------------------------
struct Guid { u32 d1; u16 d2, d3; u8 d4[8]; };
constexpr Guid kClsidMmDeviceEnumerator{0xBCDE0395, 0xE52F, 0x467C, {0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
constexpr Guid kIidIMmDeviceEnumerator{0xA95664D2, 0x9614, 0x4F35, {0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
constexpr Guid kIidIAudioClient    {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
constexpr Guid kIidIAudioRenderClient{0xF294ACFC,0x3146,0x4483,{0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2}};
bool guid_eq(const void* a, const void* b) {
    return std::memcmp(a, b, 16) == 0;
}

// ---- PulseAudio simple API (dlopen; same pattern as win32_audio.cpp) -------
struct PaSimple;
using pa_simple_new_fn   = PaSimple* (*)(const char*, const char*, int, const char*, const char*, const void*, const void*, int*);
using pa_simple_write_fn = int (*)(PaSimple*, const void*, std::size_t, int*);
using pa_simple_drain_fn = int (*)(PaSimple*, int*);
using pa_simple_free_fn  = void (*)(PaSimple*);
constexpr int PA_STREAM_PLAYBACK = 1;

struct PulseApi {
    bool ok{false};
    pa_simple_new_fn   new_pa{nullptr};
    pa_simple_write_fn write_pa{nullptr};
    pa_simple_drain_fn drain_pa{nullptr};
    pa_simple_free_fn  free_pa{nullptr};
};
PulseApi& pulse_mm() {
    static PulseApi p;
    static bool init = false;
    if (!init) {
        init = true;
        void* h = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!h) h = dlopen("libpulse-simple.so", RTLD_NOW | RTLD_LOCAL);
        if (h) {
            p.new_pa   = (pa_simple_new_fn)   dlsym(h, "pa_simple_new");
            p.write_pa = (pa_simple_write_fn) dlsym(h, "pa_simple_write");
            p.drain_pa = (pa_simple_drain_fn) dlsym(h, "pa_simple_drain");
            p.free_pa  = (pa_simple_free_fn)  dlsym(h, "pa_simple_free");
            p.ok = p.new_pa && p.write_pa;
        }
    }
    return p;
}

// ---- shared audio client state ---------------------------------------------
struct AudioClientState {
    u32 sample_rate{48000};
    u32 channels{2};
    u32 bytes_per_frame{8};      // float32 stereo
    u32 buffer_frames{0};        // frames per period (from Initialize)
    std::vector<u8> ring;        // PCM ring: 2 periods
    std::vector<u8> inbuf;       // GetBuffer staging
    std::mutex mtx;
    std::condition_variable cv;
    bool running{false};
    void* event_handle{nullptr}; // SetEvent after ReleaseBuffer
    // writer
    std::thread writer;
    PaSimple* pa{nullptr};
    std::atomic<bool> quit{false};

    void start_writer() {
        if (writer.joinable()) return;
        quit = false;
        running = true;
        writer = std::thread([this] { writer_loop(); });
    }
    void writer_loop() {
        PulseApi& p = pulse_mm();
        // pa_sample_spec: format(FLOAT32LE=3), rate, channels
        unsigned char spec[16] = {3,0, 0,0, 0,0,0,0, 0,0,0,0, 0,0, 0,0};
        std::memcpy(spec + 4, &sample_rate, 4);
        std::memcpy(spec + 12, &channels, 4);
        if (p.ok) pa = p.new_pa("papaya", "WASAPI", PA_STREAM_PLAYBACK, nullptr,
                                "papaya", spec, nullptr, nullptr);
        while (!quit) {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait_for(lk, std::chrono::milliseconds(50), [&] { return quit || ring.size() >= bytes_per_frame; });
            if (quit) break;
            std::size_t avail = ring.size();
            if (avail == 0) continue;
            // drain in up-to-8KB chunks (pa_simple_write is blocking)
            std::size_t chunk = avail > 8192 ? 8192 : avail;
            std::vector<u8> out(ring.begin(), ring.begin() + chunk);
            ring.erase(ring.begin(), ring.begin() + chunk);
            lk.unlock();
            if (pa) p.write_pa(pa, out.data(), out.size(), nullptr);
            lk.lock();
        }
        if (pa && p.ok) { if (p.drain_pa) p.drain_pa(pa, nullptr); p.free_pa(pa); pa = nullptr; }
    }
    void stop() {
        quit = true;
        cv.notify_all();
        if (writer.joinable()) writer.join();
        running = false;
    }
};
AudioClientState& ac_state() { static AudioClientState s; return s; }

// ---- COM helpers ------------------------------------------------------------
struct ComObj { void** vtbl; };
static D3DMS u64 thunk_qi(void* self, u64 riid, void* ppv) {
    if (ppv) *reinterpret_cast<void**>(ppv) = self;
    return 0; // S_OK
}
static D3DMS u64 thunk_addref(void*) { return 1; }
static D3DMS u64 thunk_release(void* self) {
    // Endpoint objects are singletons; nothing to free.
    return 0;
}

// WAVEFORMATEX: wFormatTag(0) nChannels(2) nSamplesPerSec(4) nAvgBytesPerSec(8)
// nBlockAlign(12) wBitsPerSample(14) cbSize(16)
// GetMixFormat(WAVEFORMATEX** ppFormat): WRITE a pointer into caller storage.
static u8 g_mixfmt[44];
static D3DMS u64 client_get_mix_format(void* self, void* ppfx) {
    (void)self;
    if (!ppfx) return 0x80070057;
    auto& st = ac_state();
    u8* w = g_mixfmt;
    *(u16*)(w + 0)  = 3;                       // WAVE_FORMAT_IEEE_FLOAT
    *(u16*)(w + 2)  = static_cast<u16>(st.channels);
    *(u32*)(w + 4)  = st.sample_rate;
    *(u32*)(w + 8)  = st.sample_rate * st.channels * 4;
    *(u16*)(w + 12) = static_cast<u16>(st.channels * 4);
    *(u16*)(w + 14) = 32;
    *(u16*)(w + 16) = 0;
    *reinterpret_cast<void**>(ppfx) = w;
    return 0;
}
static D3DMS u64 client_get_current_padding(void* self, void* pPad) {
    (void)self;
    if (pPad) *static_cast<u32*>(pPad) = 0;
    return 0;
}
static D3DMS u64 client_get_stream_latency(void* self, void* pLat) {
    (void)self;
    if (pLat) *static_cast<u64*>(pLat) = 100000;
    return 0;
}
static D3DMS u64 client_is_format_supported(void* self, u64 mode, void* pfmt, void* closest) {
    (void)self; (void)mode; (void)closest;
    return 0; // S_OK
}

static AudioClientState* g_client_frames_cb = nullptr;

static D3DMS u64 client_initialize(void* self, u64 share_mode, u64 flags, u64 hns_duration, u64 periodicity, void* pwfx, void* guid) {
    (void)self; (void)share_mode; (void)flags; (void)periodicity; (void)guid;
    auto& st = ac_state();
    if (pwfx) {
        u16 tag; std::memcpy(&tag, pwfx, 2);
        if (tag != 3) { // non-float: convert our state to int16
            st.channels = *(u16*)((u8*)pwfx + 2);
            st.sample_rate = *(u32*)((u8*)pwfx + 4);
            st.bytes_per_frame = *(u16*)((u8*)pwfx + 12);
        } else {
            st.channels = *(u16*)((u8*)pwfx + 2);
            st.sample_rate = *(u32*)((u8*)pwfx + 4);
            st.bytes_per_frame = st.channels * 4;
        }
    }
    // hns_duration: 100ns units; clamp 5ms..500ms
    u64 hns = hns_duration ? hns_duration : 100000; // 10ms
    if (hns > 5'000'000) hns = 5'000'000;
    if (hns < 50'000) hns = 50'000;
    u64 frames64 = (hns / 10000000ULL) * st.sample_rate;
    if (frames64 == 0) frames64 = (st.sample_rate / 100); // 10ms
    st.buffer_frames = static_cast<u32>(frames64);
    st.ring.assign(st.buffer_frames * st.bytes_per_frame * 2, 0);
    st.inbuf.assign(st.buffer_frames * st.bytes_per_frame * 2, 0);
    st.start_writer();
    return 0;
}

static D3DMS u64 client_get_buffer_size(void* self, void* pFrames) {
    (void)self;
    if (!pFrames) return 0x80070057;
    *static_cast<u32*>(pFrames) = ac_state().buffer_frames;
    return 0;
}
static D3DMS u64 client_get_device_period(void* self, void* def, void* min) {
    (void)self;
    if (def) *static_cast<u64*>(def) = 100000;   // 10ms
    if (min) *static_cast<u64*>(min) = 100000;
    return 0;
}
static D3DMS u64 client_start(void* self) { (void)self; ac_state().running = true; return 0; }
static D3DMS u64 client_stop(void* self)  { (void)self; ac_state().running = false; return 0; }
static D3DMS u64 client_reset(void* self) { (void)self; return 0; }

static D3DMS u64 client_set_event_handle(void* self, void* hEvent) {
    fprintf(stderr, "[MMDEV] SETE VT\n");
    (void)self;
    ac_state().event_handle = hEvent;
    return 0;
}

// IAudioRenderClient
static D3DMS u64 render_get_buffer(void* self, u64 frames, void* ppData) {
    (void)self;
    auto& st = ac_state();
    if (!ppData) return 0x80070057;
    *reinterpret_cast<void**>(ppData) = st.inbuf.data();
    (void)frames;
    return 0;
}
static D3DMS u64 render_release_buffer(void* self, u64 written, u64 flags) {
    (void)self; (void)flags;
    auto& st = ac_state();
    std::size_t bytes = static_cast<std::size_t>(written) * st.bytes_per_frame;
    if (bytes > st.ring.capacity()) bytes = st.ring.capacity();
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        if (st.ring.size() + bytes > st.ring.capacity()) st.ring.clear();
        st.ring.insert(st.ring.end(), st.inbuf.begin(), st.inbuf.begin() + bytes);
    }
    st.cv.notify_all();
    if (st.event_handle) Win32ApiHle::hle_set_event(st.event_handle);
    return 0;
}

// IMMDevice::Activate
static D3DMS u64 device_activate(void* self, u64 riid, u64 ctx, u64 /*params*/, void* ppv) {
    (void)self; (void)ctx;
    auto& st = ac_state();
    if (!ppv) return 0x80070057;
    if (guid_eq(reinterpret_cast<const void*>(riid), &kIidIAudioClient)) {
        static void** client_vt;
        if (!client_vt) {
            client_vt = static_cast<void**>(calloc(32, sizeof(void*)));
            for (int i = 0; i < 32; i++) client_vt[i] = reinterpret_cast<void*>(&thunk_release);
            client_vt[0] = reinterpret_cast<void*>(&thunk_qi);
            client_vt[1] = reinterpret_cast<void*>(&thunk_addref);
            client_vt[2] = reinterpret_cast<void*>(&thunk_release);
            client_vt[3] = reinterpret_cast<void*>(&client_initialize);
            client_vt[4] = reinterpret_cast<void*>(&client_get_buffer_size);
            client_vt[5] = reinterpret_cast<void*>(&client_get_stream_latency);
            client_vt[6] = reinterpret_cast<void*>(&client_get_current_padding);
            client_vt[7] = reinterpret_cast<void*>(&client_is_format_supported);
            client_vt[8] = reinterpret_cast<void*>(&client_get_mix_format);
            client_vt[9] = reinterpret_cast<void*>(&client_get_device_period);
            client_vt[10] = reinterpret_cast<void*>(&client_start);
            client_vt[11] = reinterpret_cast<void*>(&client_stop);
            client_vt[12] = reinterpret_cast<void*>(&client_reset);
            client_vt[13] = reinterpret_cast<void*>(&client_set_event_handle);
            client_vt[14] = reinterpret_cast<void*>(&thunk_release); // GetService -> see below
        }
        // GetService must return the render client; make slot 14 a custom thunk.
        // (Rebuild below so we can reference render vtbl.)
        static void** render_vt;
        if (!render_vt) {
            render_vt = static_cast<void**>(calloc(16, sizeof(void*)));
            for (int i = 0; i < 16; i++) render_vt[i] = reinterpret_cast<void*>(&thunk_release);
            render_vt[0] = reinterpret_cast<void*>(&thunk_qi);
            render_vt[1] = reinterpret_cast<void*>(&thunk_addref);
            render_vt[2] = reinterpret_cast<void*>(&thunk_release);
            render_vt[3] = reinterpret_cast<void*>(&render_get_buffer);
            render_vt[4] = reinterpret_cast<void*>(&render_release_buffer);
        }
        struct RenderObj : ComObj {};
        static RenderObj render_obj{render_vt};
        struct GetServiceThunk {
            static D3DMS u64 get_service(void* self, u64 riid, void* ppv) {
                (void)self;
                if (guid_eq(reinterpret_cast<const void*>(riid), &kIidIAudioRenderClient)) {
                    if (ppv) *reinterpret_cast<void**>(ppv) = &render_obj;
                    return 0;
                }
                return 0x80004002;
            }
        };
        client_vt[14] = reinterpret_cast<void*>(&GetServiceThunk::get_service);
        static ComObj client_obj{client_vt};
        *reinterpret_cast<void**>(ppv) = &client_obj;
        (void)st;
        return 0;
    }
    return 0x80004002;
}

static D3DMS u64 device_get_id(void* self, void* ppid) {
    (void)self;
    if (ppid) *reinterpret_cast<void**>(ppid) = nullptr;
    return 0x80070057; // not critical
}
static D3DMS u64 device_get_state(void* self, void* pstate) {
    (void)self;
    if (pstate) *static_cast<u32*>(pstate) = 1; // DEVICE_STATE_ACTIVE
    return 0;
}

// IMMDeviceEnumerator::GetDefaultAudioEndpoint
static D3DMS u64 enumerator_get_default(void* self, u64 data_flow, u64 role, void* ppDevice) {
    (void)self; (void)data_flow; (void)role;
    if (!ppDevice) return 0x80070057;
    static void** device_vt;
    if (!device_vt) {
        device_vt = static_cast<void**>(calloc(16, sizeof(void*)));
        for (int i = 0; i < 16; i++) device_vt[i] = reinterpret_cast<void*>(&thunk_release);
        device_vt[0] = reinterpret_cast<void*>(&thunk_qi);
        device_vt[1] = reinterpret_cast<void*>(&thunk_addref);
        device_vt[2] = reinterpret_cast<void*>(&thunk_release);
        device_vt[3] = reinterpret_cast<void*>(&device_activate);
        device_vt[4] = reinterpret_cast<void*>(&thunk_release); // OpenPropertyStore
        device_vt[5] = reinterpret_cast<void*>(&device_get_id);
        device_vt[6] = reinterpret_cast<void*>(&device_get_state);
    }
    static ComObj device_obj{device_vt};
    *reinterpret_cast<void**>(ppDevice) = &device_obj;
    return 0;
}

} // namespace

// Host-side guard: CoTaskMemFree must never free our static WAVEFORMATEX.
bool mmdevice_is_static_ptr(const void* p) {
    return p == g_mixfmt;
}

// Called from hle_co_create_instance for CLSID_MMDeviceEnumerator.
bool mmdevice_try_create(const void* rclsid, const void* riid, void** ppv) {
    static void** enumerator_vt;
    if (!enumerator_vt) {
        enumerator_vt = static_cast<void**>(calloc(16, sizeof(void*)));
        for (int i = 0; i < 16; i++) enumerator_vt[i] = reinterpret_cast<void*>(&thunk_release);
        enumerator_vt[0] = reinterpret_cast<void*>(&thunk_qi);
        enumerator_vt[1] = reinterpret_cast<void*>(&thunk_addref);
        enumerator_vt[2] = reinterpret_cast<void*>(&thunk_release);
        enumerator_vt[3] = reinterpret_cast<void*>(&thunk_release); // EnumAudioEndpoints
        enumerator_vt[4] = reinterpret_cast<void*>(&enumerator_get_default);
        enumerator_vt[5] = reinterpret_cast<void*>(&thunk_release); // GetDevice
    }
    static struct EnumeratorObj : ComObj {} enumerator_obj{enumerator_vt};
    if (guid_eq(rclsid, &kClsidMmDeviceEnumerator) && guid_eq(riid, &kIidIMmDeviceEnumerator)) {
        if (ppv) *ppv = &enumerator_obj;
        return true;
    }
    return false;
}

} // namespace papaya::win32