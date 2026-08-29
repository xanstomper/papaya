#include "papaya/win32/win32_api_hle.hpp"
#include "papaya/win32/pe_loader.hpp"
#include "papaya/common/logger.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <fcntl.h>
#include <time.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <csignal>
#include <cwchar>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <algorithm>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <dlfcn.h>

namespace papaya::win32 {

static Win32ApiHle* g_active_win32_hle = nullptr;
thread_local u32 g_last_error = 0;
static std::vector<pthread_key_t> g_tls_keys;
static std::mutex g_tls_mutex;
static std::shared_ptr<steam::SteamApiStub> g_active_steam_stub = nullptr;
static std::shared_ptr<input::VirtualXInputManager> g_active_input_mgr = nullptr;

// ---------------------------------------------------------------------------
// Data-import targets. msvcrt exports data symbols (__initenv, _environ, ...)
// that the CRT dereferences as addresses and writes through. They must resolve
// to REAL, writable, non-null variables — NOT function stubs. The CRT does
// e.g. `mov (%__imp___initenv),rax; mov envp,(rax)` so __initenv itself must
// point at a valid char* array (else the write-through faults on NULL).
// ---------------------------------------------------------------------------
static char* g_initenv_slots[2] = { nullptr, nullptr };
static char** g_initenv = g_initenv_slots;      // __initenv -> empty env array
static char*  g_environ_slots[2] = { nullptr, nullptr };
static char** g_environ = g_environ_slots;      // _environ  -> empty env array
// Command-line string that main()/CRT parse directly via _acmdln/__p__acmdln.
static char  g_acmdln_buf[] = "papaya_game.exe";
static char* g_acmdln = g_acmdln_buf;           // _acmdln -> non-null cmdline

// CRT error globals whose accessors (msvcrt! _errno/__p__errno/_doserrno)
// return POINTERS to them; the CRT dereferences the returned pointer.
static thread_local int g_crt_errno = 0;
static thread_local unsigned long g_crt_doserrno = 0;
static thread_local int* g_p_errno_slot = nullptr; // __p__errno -> &int*

// ---------------------------------------------------------------------------
// Standard I/O (_iob) emulation.
// The CRT reaches stdout/stdin/stderr via __iob_func() (imported from msvcrt)
// which returns the base of the _iob[] array. __acrt_iob_func(i) then computes
// base + i*48 (mingw's FILE is 48 bytes on the wire). __mingw_pformat writes to
// _iob[1] (stdout) through this. So __iob_func() must hand back a buffer of at
// least 6 * 48 bytes whose slots alias host stdin/stdout/stderr.
//
// Each 48-byte guest _iob slot stores a pointer to the host FILE in its first
// 8 bytes (mirroring where FILE::_ptr lives). Our stdio HLE functions read that
// back to recover the host stream. Buffers are never read/written structurally
// by us — the slot is an opaque handle target.
// ---------------------------------------------------------------------------
constexpr size_t kGuestFileStride  = 48;   // WIN rt FILE stride (24*2)
constexpr size_t kGuestIOBSlots    = 6;    // stdin..stderr + pseudo
static unsigned char g_iob_slots[kGuestIOBSlots * kGuestFileStride] alignas(8);

static void* iob_at(int idx) { return g_iob_slots + static_cast<size_t>(idx) * kGuestFileStride; }

// Map a guest FILE* (lodged in a guest _iob slot) back to the host FILE*.
static FILE* host_file_for(void* guest_file) {
    unsigned char* slot = static_cast<unsigned char*>(guest_file);
    // Clamp to the containing 48-byte slot base.
    if (slot >= g_iob_slots && slot < g_iob_slots + sizeof(g_iob_slots)) {
        size_t off = static_cast<size_t>(slot - g_iob_slots);
        slot = g_iob_slots + (off / kGuestFileStride) * kGuestFileStride;
    }
    FILE* hf = nullptr;
    std::memcpy(&hf, slot, sizeof(hf));
    return hf;
}
static FILE* host_stream(int idx) {
    FILE* hf = nullptr;
    std::memcpy(&hf, g_iob_slots + static_cast<size_t>(idx) * kGuestFileStride, sizeof(hf));
    return hf;
}

// Generic no-op stub for uncritical APIs
static PAPAYA_MS_ABI void* generic_stub_success() { return reinterpret_cast<void*>(1); }
static PAPAYA_MS_ABI void* generic_stub_null() { return nullptr; }
static PAPAYA_MS_ABI int   generic_stub_zero() { return 0; }
static PAPAYA_MS_ABI void* generic_stub_arg0(void* p) { return p; }
static PAPAYA_MS_ABI int   hle_bcrypt_gen_random(void* hAlg, u8* pbBuffer, u32 cbBuffer, u32 dwFlags) {
    if (pbBuffer && cbBuffer > 0) {
        if (getrandom(pbBuffer, cbBuffer, 0) < 0) {
            for (u32 i = 0; i < cbBuffer; ++i) pbBuffer[i] = static_cast<u8>(rand() & 0xFF);
        }
    }
    return 0; // STATUS_SUCCESS
}

// -------------------------------------------------------------
// Memory Emulation
// -------------------------------------------------------------
void* Win32ApiHle::hle_virtual_alloc(void* lpAddress, size_t dwSize, u32 flAllocationType, u32 flProtect) {
    int prot = PROT_READ | PROT_WRITE;
    if (flProtect == 0x40 || flProtect == 0x20) prot |= PROT_EXEC;

    void* ptr = mmap(lpAddress, dwSize, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        g_last_error = 8;
        return nullptr;
    }
    return ptr;
}

BOOL Win32ApiHle::hle_virtual_free(void* lpAddress, size_t dwSize, u32 dwFreeType) {
    if (!lpAddress) return FALSE_VAL;
    munmap(lpAddress, dwSize);
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_virtual_protect(void* lpAddress, size_t dwSize, u32 flNewProtect, u32* lpflOldProtect) {
    if (lpflOldProtect) *lpflOldProtect = 0x04;
    int prot = PROT_READ | PROT_WRITE;
    if (flNewProtect == 0x40 || flNewProtect == 0x20) prot |= PROT_EXEC;
    return (mprotect(lpAddress, dwSize, prot) == 0) ? TRUE_VAL : FALSE_VAL;
}

HANDLE Win32ApiHle::hle_get_process_heap() {
    return reinterpret_cast<HANDLE>(0x1000);
}

void* Win32ApiHle::hle_heap_alloc(HANDLE hHeap, u32 dwFlags, size_t dwBytes) {
    return std::malloc(dwBytes);
}

BOOL Win32ApiHle::hle_heap_free(HANDLE hHeap, u32 dwFlags, void* lpMem) {
    if (lpMem) std::free(lpMem);
    return TRUE_VAL;
}

void* Win32ApiHle::hle_heap_realloc(HANDLE hHeap, u32 dwFlags, void* lpMem, size_t dwBytes) {
    return std::realloc(lpMem, dwBytes);
}

void* Win32ApiHle::hle_local_alloc(u32 uFlags, size_t uBytes) {
    return std::malloc(uBytes);
}

void* Win32ApiHle::hle_local_free(void* hMem) {
    if (hMem) std::free(hMem);
    return nullptr;
}

// -------------------------------------------------------------
// TLS & Threading Emulation
// -------------------------------------------------------------
u32 Win32ApiHle::hle_tls_alloc() {
    std::lock_guard<std::mutex> lock(g_tls_mutex);
    pthread_key_t key;
    if (pthread_key_create(&key, nullptr) == 0) {
        g_tls_keys.push_back(key);
        return static_cast<u32>(g_tls_keys.size() - 1);
    }
    return 0xFFFFFFFF;
}

BOOL Win32ApiHle::hle_tls_free(u32 dwTlsIndex) {
    std::lock_guard<std::mutex> lock(g_tls_mutex);
    if (dwTlsIndex < g_tls_keys.size()) {
        pthread_key_delete(g_tls_keys[dwTlsIndex]);
        return TRUE_VAL;
    }
    return FALSE_VAL;
}

void* Win32ApiHle::hle_tls_get_value(u32 dwTlsIndex) {
    std::lock_guard<std::mutex> lock(g_tls_mutex);
    if (dwTlsIndex < g_tls_keys.size()) {
        return pthread_getspecific(g_tls_keys[dwTlsIndex]);
    }
    return nullptr;
}

BOOL Win32ApiHle::hle_tls_set_value(u32 dwTlsIndex, void* lpTlsValue) {
    std::lock_guard<std::mutex> lock(g_tls_mutex);
    if (dwTlsIndex < g_tls_keys.size()) {
        return (pthread_setspecific(g_tls_keys[dwTlsIndex], lpTlsValue) == 0) ? TRUE_VAL : FALSE_VAL;
    }
    return FALSE_VAL;
}

// Handle-type tags so wait/close dispatch correctly (thread vs event vs mutex).
enum : u32 {
    kHandleNone   = 0,
    kHandleThread = 0x54524844,  // "THRD"
    kHandleEvent  = 0x45564E54,  // "EVNT"
    kHandleMutex  = 0x4D545854,  // "MTXT"
    kHandleSem    = 0x53454D41,  // "SEMA"
};
static u32 handle_tag(void* h) {
    if (!h) return kHandleNone;
    // fd-encoded file handles are small integers cast to pointers; never tag them.
    if (reinterpret_cast<uintptr_t>(h) < 0x10000) return kHandleNone;
    u32 tag = *reinterpret_cast<u32*>(h);
    // Validate against the exact set of known tags (values are not monotonic).
    switch (tag) {
        case kHandleThread: case kHandleEvent: case kHandleMutex: case kHandleSem:
            return tag;
        default:
            return kHandleNone;
    }
}

struct NativeThreadState {
    u32       tag{kHandleThread};
    pthread_t handle{};
};

struct ThreadParamBridge {
    void* lpStart;
    void* lpParam;
};

static void* thread_trampoline(void* arg) {
    auto* tp = static_cast<ThreadParamBridge*>(arg);
    auto fn = reinterpret_cast<u32 (__attribute__((ms_abi))*)(void*)>(tp->lpStart);
    void* param = tp->lpParam;
    delete tp;
    // Give the new guest thread its own per-thread TEB + TLS block so
    // Win32 TLS API and __declspec(thread) are thread-local, not shared.
    if (auto* loader = PeLoader::active()) loader->setup_thread_tls();
    u32 ret = fn(param);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(ret));
}

HANDLE Win32ApiHle::hle_create_thread(void* lpSec, size_t dwStack, void* lpStart, void* lpParam, u32 dwFlags, u32* lpId) {
    pthread_t thread;
    auto* tp = new ThreadParamBridge{lpStart, lpParam};
    if (pthread_create(&thread, nullptr, thread_trampoline, tp) == 0) {
        auto* nts = new NativeThreadState();
        nts->tag = kHandleThread;
        nts->handle = thread;
        if (lpId) *lpId = static_cast<u32>(thread);
        return reinterpret_cast<HANDLE>(nts);
    }
    delete tp;
    return nullptr;
}

u32 Win32ApiHle::hle_get_current_thread_id() {
    return static_cast<u32>(gettid());
}

u32 Win32ApiHle::hle_get_current_process_id() {
    return static_cast<u32>(getpid());
}

HANDLE Win32ApiHle::hle_get_current_thread() {
    return reinterpret_cast<HANDLE>(pthread_self());
}

HANDLE Win32ApiHle::hle_get_current_process() {
    return reinterpret_cast<HANDLE>(reinterpret_cast<void*>(-1));
}

void Win32ApiHle::hle_exit_process(u32 uExitCode) {
    log::info("WIN32", "Game invoked ExitProcess({})", uExitCode);
    _exit(static_cast<int>(uExitCode));
}

void Win32ApiHle::hle_exit_thread(u32 dwExitCode) {
    pthread_exit(reinterpret_cast<void*>(static_cast<uintptr_t>(dwExitCode)));
}

void Win32ApiHle::hle_sleep(u32 dwMilliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(dwMilliseconds));
}

u32 Win32ApiHle::hle_sleep_ex(u32 dwMilliseconds, BOOL bAlertable) {
    std::this_thread::sleep_for(std::chrono::milliseconds(dwMilliseconds));
    return 0;
}

BOOL Win32ApiHle::hle_switch_to_thread() {
    sched_yield();
    return TRUE_VAL;
}

// -------------------------------------------------------------
// Synchronization
// -------------------------------------------------------------
void Win32ApiHle::hle_init_critical_section(Win32CriticalSection* lpSection) {
    if (!lpSection) return;
    auto* mtx = new std::recursive_mutex();
    lpSection->DebugInfo = mtx;
    lpSection->LockCount = -1;
    lpSection->RecursionCount = 0;
}

BOOL Win32ApiHle::hle_init_critical_section_and_spin_count(Win32CriticalSection* lpSection, u32 dwSpinCount) {
    hle_init_critical_section(lpSection);
    if (lpSection) lpSection->SpinCount = dwSpinCount;
    return TRUE_VAL;
}

void Win32ApiHle::hle_enter_critical_section(Win32CriticalSection* lpSection) {
    if (!lpSection || !lpSection->DebugInfo) return;
    auto* mtx = static_cast<std::recursive_mutex*>(lpSection->DebugInfo);
    mtx->lock();
}

BOOL Win32ApiHle::hle_try_enter_critical_section(Win32CriticalSection* lpSection) {
    if (!lpSection || !lpSection->DebugInfo) return FALSE_VAL;
    auto* mtx = static_cast<std::recursive_mutex*>(lpSection->DebugInfo);
    return mtx->try_lock() ? TRUE_VAL : FALSE_VAL;
}

void Win32ApiHle::hle_leave_critical_section(Win32CriticalSection* lpSection) {
    if (!lpSection || !lpSection->DebugInfo) return;
    auto* mtx = static_cast<std::recursive_mutex*>(lpSection->DebugInfo);
    mtx->unlock();
}

void Win32ApiHle::hle_delete_critical_section(Win32CriticalSection* lpSection) {
    if (!lpSection || !lpSection->DebugInfo) return;
    auto* mtx = static_cast<std::recursive_mutex*>(lpSection->DebugInfo);
    delete mtx;
    lpSection->DebugInfo = nullptr;
}

struct NativeEventState {
    u32      tag{kHandleEvent};
    std::mutex mtx;
    std::condition_variable cv;
    bool signaled{false};
    bool manual_reset{false};
};

HANDLE Win32ApiHle::hle_create_event_a(void* lpSec, BOOL bManualReset, BOOL bInitialState, const char* lpName) {
    auto* ev = new NativeEventState();
    ev->signaled = (bInitialState != 0);
    ev->manual_reset = (bManualReset != 0);
    return reinterpret_cast<HANDLE>(ev);
}

HANDLE Win32ApiHle::hle_create_event_w(void* lpSec, BOOL bManualReset, BOOL bInitialState, const wchar_t* lpName) {
    return hle_create_event_a(lpSec, bManualReset, bInitialState, nullptr);
}

BOOL Win32ApiHle::hle_set_event(HANDLE hEvent) {
    if (!hEvent) return FALSE_VAL;
    auto* ev = static_cast<NativeEventState*>(hEvent);
    {
        std::lock_guard<std::mutex> lock(ev->mtx);
        ev->signaled = true;
    }
    ev->cv.notify_all();
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_reset_event(HANDLE hEvent) {
    if (!hEvent) return FALSE_VAL;
    auto* ev = static_cast<NativeEventState*>(hEvent);
    std::lock_guard<std::mutex> lock(ev->mtx);
    ev->signaled = false;
    return TRUE_VAL;
}

HANDLE Win32ApiHle::hle_create_mutex_a(void* lpSec, BOOL bInitialOwner, const char* lpName) {
    auto* mtx = new std::mutex();
    return reinterpret_cast<HANDLE>(mtx);
}

BOOL Win32ApiHle::hle_release_mutex(HANDLE hMutex) {
    if (!hMutex) return FALSE_VAL;
    auto* mtx = static_cast<std::mutex*>(hMutex);
    mtx->unlock();
    return TRUE_VAL;
}

u32 Win32ApiHle::hle_wait_for_single_object(HANDLE hHandle, u32 dwMilliseconds) {
    if (!hHandle) return 0xFFFFFFFF;
    switch (handle_tag(hHandle)) {
        case kHandleThread: {
            auto* th = static_cast<NativeThreadState*>(hHandle);
            // Wait on a thread handle == join (bounded by timeout).
            u32 delay_ms = (dwMilliseconds == 0xFFFFFFFF) ? 30'000 : (dwMilliseconds ? dwMilliseconds : 0);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += static_cast<long>(delay_ms) * 1'000'000ULL;
            ts.tv_sec  += ts.tv_nsec / 1'000'000'000ULL;
            ts.tv_nsec %= 1'000'000'000ULL;
            int rc = pthread_timedjoin_np(th->handle, nullptr, &ts);
            if (rc == 0) return 0;            // thread finished
            if (rc == ETIMEDOUT) return 0x102; // still running
            return 0;
        }
        case kHandleEvent: {
            auto* ev = static_cast<NativeEventState*>(hHandle);
            std::unique_lock<std::mutex> lock(ev->mtx);
            if (ev->signaled) { if (!ev->manual_reset) ev->signaled = false; return 0; }
            if (dwMilliseconds == 0) return 0x102;
            if (dwMilliseconds == 0xFFFFFFFF) {
                ev->cv.wait(lock, [&]() { return ev->signaled; });
                if (!ev->manual_reset) ev->signaled = false;
                return 0;
            }
            bool res = ev->cv.wait_for(lock, std::chrono::milliseconds(dwMilliseconds),
                                       [&]() { return ev->signaled; });
            if (res) { if (!ev->manual_reset) ev->signaled = false; return 0; }
            return 0x102;
        }
        default:
            return 0xFFFFFFFF;   // unsupported / mutex (mutex wait not via this path)
    }
}

u32 Win32ApiHle::hle_wait_for_multiple_objects(u32 nCount, const HANDLE* lpHandles, BOOL bWaitAll, u32 dwMilliseconds) {
    for (u32 i = 0; i < nCount; ++i) {
        u32 res = hle_wait_for_single_object(lpHandles[i], dwMilliseconds);
        if (res == 0 && !bWaitAll) return i;
    }
    return 0;
}

// -------------------------------------------------------------
// File System & Paths
// -------------------------------------------------------------
static std::string normalize_win_path(const char* p) {
    if (!p) return "";
    std::string s(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

HANDLE Win32ApiHle::hle_create_file_a(const char* lpFileName, u32 dwAccess, u32 dwShare, void* lpSec, u32 dwDisp, u32 dwFlags, HANDLE hTemplate) {
    std::string path = normalize_win_path(lpFileName);
    int flags = O_RDWR;
    if (dwDisp == 2) flags |= O_CREAT | O_TRUNC;
    else if (dwDisp == 4) flags |= O_CREAT;

    int fd = open(path.c_str(), flags, 0666);
    if (fd < 0) {
        fd = open(path.c_str(), O_RDONLY);
    }
    if (fd < 0) {
        g_last_error = 2;
        return reinterpret_cast<HANDLE>(reinterpret_cast<void*>(-1));
    }
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fd));
}

HANDLE Win32ApiHle::hle_create_file_w(const wchar_t* lpFileName, u32 dwAccess, u32 dwShare, void* lpSec, u32 dwDisp, u32 dwFlags, HANDLE hTemplate) {
    return hle_create_file_a("file.bin", dwAccess, dwShare, lpSec, dwDisp, dwFlags, hTemplate);
}

BOOL Win32ApiHle::hle_read_file(HANDLE hFile, void* lpBuffer, u32 nNumberOfBytesToRead, u32* lpNumberOfBytesRead, void* lpOverlapped) {
    int fd = static_cast<int>(reinterpret_cast<uintptr_t>(hFile));
    if (fd < 0 || !lpBuffer) return FALSE_VAL;

    ssize_t n = read(fd, lpBuffer, nNumberOfBytesToRead);
    if (n >= 0) {
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = static_cast<u32>(n);
        return TRUE_VAL;
    }
    return FALSE_VAL;
}

BOOL Win32ApiHle::hle_write_file(HANDLE hFile, const void* lpBuffer, u32 nNumberOfBytesToWrite, u32* lpNumberOfBytesWritten, void* lpOverlapped) {
    int fd = static_cast<int>(reinterpret_cast<uintptr_t>(hFile));
    if (fd < 0 || !lpBuffer) return FALSE_VAL;

    ssize_t n = write(fd, lpBuffer, nNumberOfBytesToWrite);
    if (n >= 0) {
        if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = static_cast<u32>(n);
        return TRUE_VAL;
    }
    return FALSE_VAL;
}

BOOL Win32ApiHle::hle_close_handle(HANDLE hObject) {
    if (!hObject) return TRUE_VAL;
    // Tagged objects (thread/sync): free the wrapper; the underlying pthread
    // is detached and reaped on exit, so CloseHandle doesn't join.
    switch (handle_tag(hObject)) {
        case kHandleThread: { delete static_cast<NativeThreadState*>(hObject); return TRUE_VAL; }
        case kHandleEvent:  { delete static_cast<NativeEventState*>(hObject); return TRUE_VAL; }
        default: break;
    }
    int fd = static_cast<int>(reinterpret_cast<uintptr_t>(hObject));
    if (fd > 2 && fd < 65536) {
        close(fd);
    }
    return TRUE_VAL;
}

u32 Win32ApiHle::hle_get_file_size(HANDLE hFile, u32* lpFileSizeHigh) {
    int fd = static_cast<int>(reinterpret_cast<uintptr_t>(hFile));
    struct stat st{};
    if (fstat(fd, &st) == 0) {
        if (lpFileSizeHigh) *lpFileSizeHigh = static_cast<u32>(st.st_size >> 32);
        return static_cast<u32>(st.st_size & 0xFFFFFFFF);
    }
    return 0xFFFFFFFF;
}

u32 Win32ApiHle::hle_set_file_pointer(HANDLE hFile, s32 lDistanceToMove, s32* lpDistanceToMoveHigh, u32 dwMoveMethod) {
    int fd = static_cast<int>(reinterpret_cast<uintptr_t>(hFile));
    int whence = SEEK_SET;
    if (dwMoveMethod == 1) whence = SEEK_CUR;
    else if (dwMoveMethod == 2) whence = SEEK_END;
    off_t res = lseek(fd, lDistanceToMove, whence);
    return static_cast<u32>(res);
}

u32 Win32ApiHle::hle_get_file_attributes_a(const char* lpFileName) {
    std::string p = normalize_win_path(lpFileName);
    struct stat st{};
    if (stat(p.c_str(), &st) == 0) {
        u32 attr = 0x80;
        if (S_ISDIR(st.st_mode)) attr |= 0x10;
        return attr;
    }
    return 0xFFFFFFFF;
}

u32 Win32ApiHle::hle_get_file_attributes_w(const wchar_t* lpFileName) {
    return 0x80;
}

BOOL Win32ApiHle::hle_get_full_path_name_a(const char* lpFileName, u32 nBufferLength, char* lpBuffer, char** lpFilePart) {
    std::string p = normalize_win_path(lpFileName);
    std::strncpy(lpBuffer, p.c_str(), nBufferLength);
    return static_cast<BOOL>(p.size());
}

u32 Win32ApiHle::hle_get_current_directory_a(u32 nBufferLength, char* lpBuffer) {
    if (getcwd(lpBuffer, nBufferLength)) {
        return static_cast<u32>(std::strlen(lpBuffer));
    }
    return 0;
}

BOOL Win32ApiHle::hle_set_current_directory_a(const char* lpPathName) {
    std::string p = normalize_win_path(lpPathName);
    return (chdir(p.c_str()) == 0) ? TRUE_VAL : FALSE_VAL;
}

HANDLE Win32ApiHle::hle_find_first_file_a(const char* lpFileName, Win32FileFindDataA* lpFindFileData) {
    if (lpFindFileData) {
        std::strncpy(lpFindFileData->cFileName, "file.dat", 259);
        lpFindFileData->dwFileAttributes = 0x80;
    }
    return reinterpret_cast<HANDLE>(0x500);
}

BOOL Win32ApiHle::hle_find_next_file_a(HANDLE hFindFile, Win32FileFindDataA* lpFindFileData) {
    return FALSE_VAL;
}

BOOL Win32ApiHle::hle_find_close(HANDLE hFindFile) {
    return TRUE_VAL;
}

// -------------------------------------------------------------
// Module, Clock & System
// -------------------------------------------------------------
void* Win32ApiHle::hle_get_proc_address(void* hModule, const char* lpProcName) {
    if (!lpProcName) {
        g_last_error = 127; // ERROR_PROC_NOT_FOUND
        return nullptr;
    }

    uintptr_t proc_val = reinterpret_cast<uintptr_t>(lpProcName);
    std::string symbol_name;
    if (proc_val < 0x10000) {
        symbol_name = std::to_string(proc_val);
    } else {
        symbol_name = lpProcName;
    }

    if (g_active_win32_hle) {
        for (const auto& [dll, funcs] : g_active_win32_hle->export_table_) {
            auto it = funcs.find(symbol_name);
            if (it != funcs.end()) {
                return it->second;
            }
        }
    }

    if (symbol_name.starts_with("gl") || symbol_name.starts_with("wgl")) {
        void* p = hle_wgl_get_proc_address(symbol_name.c_str());
        if (p) return p;
    }
    if (symbol_name.starts_with("vk")) {
        void* p = hle_vk_get_instance_proc_addr(nullptr, symbol_name.c_str());
        if (p) return p;
    }

    log::debug("HLE", "GetProcAddress: '{}' not found, returning generic fallback", symbol_name);
    return reinterpret_cast<void*>(&generic_stub_zero);
}

void* Win32ApiHle::hle_get_module_handle_a(const char* lpModuleName) {
    if (!lpModuleName) return reinterpret_cast<void*>(0x140000000); // Main EXE handle
    std::string name(lpModuleName);
    for (auto& c : name) c = static_cast<char>(std::tolower(c));
    if (name.find("kernel32") != std::string::npos) return reinterpret_cast<void*>(0x7FFF00010000);
    if (name.find("ntdll") != std::string::npos) return reinterpret_cast<void*>(0x7FFF00020000);
    if (name.find("user32") != std::string::npos) return reinterpret_cast<void*>(0x7FFF00030000);
    if (name.find("gdi32") != std::string::npos) return reinterpret_cast<void*>(0x7FFF00040000);
    if (name.find("msvcrt") != std::string::npos || name.find("ucrtbase") != std::string::npos) return reinterpret_cast<void*>(0x7FFF00050000);
    if (name.find("steam_api") != std::string::npos) return reinterpret_cast<void*>(0x7FFF00060000);
    if (name.find("xinput") != std::string::npos) return reinterpret_cast<void*>(0x7FFF00070000);
    if (name.find("opengl32") != std::string::npos) return reinterpret_cast<void*>(0x7FFF00080000);
    if (name.find("vulkan") != std::string::npos) return reinterpret_cast<void*>(0x7FFF00090000);
    if (name.find("ws2_32") != std::string::npos || name.find("wsock32") != std::string::npos) return reinterpret_cast<void*>(0x7FFF000A0000);
    if (name.find("advapi32") != std::string::npos) return reinterpret_cast<void*>(0x7FFF000B0000);
    if (name.find("shell32") != std::string::npos) return reinterpret_cast<void*>(0x7FFF000C0000);
    if (name.find("ole32") != std::string::npos || name.find("oleaut32") != std::string::npos) return reinterpret_cast<void*>(0x7FFF000D0000);
    if (name.find("winmm") != std::string::npos) return reinterpret_cast<void*>(0x7FFF000E0000);
    if (name.find("d3d11") != std::string::npos) return reinterpret_cast<void*>(0x7FFF000F0000);
    if (name.find("dxgi") != std::string::npos) return reinterpret_cast<void*>(0x7FFF00100000);
    return reinterpret_cast<void*>(0x7FFF00200000);
}

void* Win32ApiHle::hle_get_module_handle_w(const wchar_t* lpModuleName) {
    if (!lpModuleName) return reinterpret_cast<void*>(0x140000000);
    char narrow[256] = {0};
    hle_wide_char_to_multi_byte(0, 0, lpModuleName, -1, narrow, 255, nullptr, nullptr);
    return hle_get_module_handle_a(narrow);
}

void* Win32ApiHle::hle_load_library_a(const char* lpLibFileName) {
    return hle_get_module_handle_a(lpLibFileName);
}

void* Win32ApiHle::hle_load_library_w(const wchar_t* lpLibFileName) {
    return hle_get_module_handle_w(lpLibFileName);
}

void* Win32ApiHle::hle_load_library_ex_a(const char* lpLibFileName, HANDLE hFile, u32 dwFlags) {
    return hle_load_library_a(lpLibFileName);
}

void* Win32ApiHle::hle_load_library_ex_w(const wchar_t* lpLibFileName, HANDLE hFile, u32 dwFlags) {
    return hle_load_library_w(lpLibFileName);
}

BOOL Win32ApiHle::hle_free_library(void* hLibModule) {
    return TRUE_VAL;
}

u32 Win32ApiHle::hle_get_module_file_name_a(void* hModule, char* lpFilename, u32 nSize) {
    static const char* p = "C:\\papaya_game.exe";
    if (lpFilename && nSize > 0) {
        std::strncpy(lpFilename, p, nSize);
        return static_cast<u32>(std::strlen(lpFilename));
    }
    return 0;
}

u32 Win32ApiHle::hle_get_module_file_name_w(void* hModule, wchar_t* lpFilename, u32 nSize) {
    static const wchar_t* p = L"C:\\papaya_game.exe";
    if (lpFilename && nSize > 0) {
        std::wcsncpy(lpFilename, p, nSize);
        return static_cast<u32>(std::wcslen(lpFilename));
    }
    return 0;
}

void Win32ApiHle::hle_get_system_info(Win32SystemInfo* lpSystemInfo) {
    if (!lpSystemInfo) return;
    *lpSystemInfo = Win32SystemInfo{};
    lpSystemInfo->dwNumberOfProcessors = static_cast<u32>(sysconf(_SC_NPROCESSORS_ONLN));
}

void Win32ApiHle::hle_get_native_system_info(Win32SystemInfo* lpSystemInfo) {
    hle_get_system_info(lpSystemInfo);
}

BOOL Win32ApiHle::hle_is_processor_feature_present(u32 ProcessorFeature) {
    return TRUE_VAL;
}

static const char* g_cmdline = "papaya_game.exe";
static const wchar_t* g_wcmdline = L"papaya_game.exe";

const char* Win32ApiHle::hle_get_command_line_a() { return g_cmdline; }
const wchar_t* Win32ApiHle::hle_get_command_line_w() { return g_wcmdline; }

u32 Win32ApiHle::hle_get_environment_variable_a(const char* lpName, char* lpBuffer, u32 nSize) {
    const char* val = getenv(lpName);
    if (val && lpBuffer && nSize > 0) {
        std::strncpy(lpBuffer, val, nSize);
        return static_cast<u32>(std::strlen(val));
    }
    return 0;
}

BOOL Win32ApiHle::hle_query_performance_counter(s64* lpPerformanceCount) {
    if (!lpPerformanceCount) return FALSE_VAL;
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *lpPerformanceCount = static_cast<s64>(ts.tv_sec) * 1000000000LL + static_cast<s64>(ts.tv_nsec);
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_query_performance_frequency(s64* lpFrequency) {
    if (!lpFrequency) return FALSE_VAL;
    *lpFrequency = 1000000000LL;
    return TRUE_VAL;
}

u32 Win32ApiHle::hle_get_tick_count() {
    return static_cast<u32>(hle_get_tick_count_64() & 0xFFFFFFFF);
}

u64 Win32ApiHle::hle_get_tick_count_64() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000ULL + static_cast<u64>(ts.tv_nsec) / 1000000ULL;
}

u32 Win32ApiHle::hle_get_last_error() {
    return g_last_error;
}

void Win32ApiHle::hle_set_last_error(u32 dwErrCode) {
    g_last_error = dwErrCode;
}

// -------------------------------------------------------------
// USER32 & GDI32 Emulation
// -------------------------------------------------------------
s32 Win32ApiHle::hle_get_system_metrics(s32 nIndex) {
    // SM_CXSCREEN / SM_CYSCREEN from the X root window when available.
    auto* wm = &window_manager();
    (void)wm;
    if (nIndex == 0) return 1920;
    if (nIndex == 1) return 1080;
    if (nIndex == 11) return 1920; // SM_CXFULLSCREEN
    if (nIndex == 12) return 1080; // SM_CYFULLSCREEN
    return 0;
}

BOOL Win32ApiHle::hle_set_process_dpi_aware() { return TRUE_VAL; }

BOOL Win32ApiHle::hle_get_client_rect(HWND hWnd, void* lpRect) {
    window_manager().get_client_rect(hWnd, lpRect);
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_get_window_rect(HWND hWnd, void* lpRect) {
    window_manager().get_window_rect(hWnd, lpRect);
    return TRUE_VAL;
}

// ---- Window classes / creation ----
void* Win32ApiHle::hle_register_class_a(const void* lpWndClass) {
    if (!lpWndClass) return nullptr;
    // mingw WNDCLASSA (x64): wndproc @8, class name @64.
    const auto* buf = static_cast<const u8*>(lpWndClass);
    void* wndproc  = nullptr;
    std::memcpy(&wndproc, buf + 8, sizeof(wndproc));
    const char* cls_name = nullptr;
    std::memcpy(&cls_name, buf + 64, sizeof(cls_name));
    void* hinst = nullptr;
    std::memcpy(&hinst, buf + 24, sizeof(hinst));
    if (!cls_name) return nullptr;
    return window_manager().register_class(cls_name, wndproc, hinst);
}

void* Win32ApiHle::hle_create_window_ex_a(u32 dwExStyle, const char* lpClassName,
             const char* lpWindowName, u32 dwStyle, int x, int y, int w, int h,
             void* hWndParent, void* hMenu, void* hInstance, void* lpParam) {
    (void)dwExStyle; (void)hMenu; (void)lpParam;
    window_manager().initialize();
    return window_manager().create_window_ex(lpClassName, lpWindowName, dwStyle,
                                             x, y, w, h, hWndParent, hInstance, lpParam, false);
}

BOOL Win32ApiHle::hle_destroy_window(HWND hWnd) {
    window_manager().destroy_window(hWnd);
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_show_window(HWND hWnd, int nCmdShow) {
    window_manager().show_window(hWnd, nCmdShow);
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_update_window(HWND hWnd) {
    window_manager().update_window(hWnd);
    return TRUE_VAL;
}
s64 Win32ApiHle::hle_def_window_proc_a(HWND hWnd, u32 msg, u64 wParam, s64 lParam) {
    return window_manager().def_window_proc(hWnd, msg, wParam, lParam);
}
void Win32ApiHle::hle_post_quit_message(int nExitCode) {
    window_manager().post_quit_message(nExitCode);
}
BOOL Win32ApiHle::hle_post_message_a(HWND hWnd, u32 msg, u64 wParam, s64 lParam) {
    return window_manager().post_message_a(hWnd, msg, wParam, lParam) ? TRUE_VAL : FALSE_VAL;
}
s64 Win32ApiHle::hle_send_message_a(HWND hWnd, u32 msg, u64 wParam, s64 lParam) {
    return window_manager().send_message_a(hWnd, msg, wParam, lParam);
}
void* Win32ApiHle::hle_get_dc(HWND hWnd) { return window_manager().get_dc(hWnd); }
int   Win32ApiHle::hle_release_dc(HWND hWnd, void* hDC) { return window_manager().release_dc(hWnd, hDC); }

// ---- Message pump ----
int Win32ApiHle::hle_get_message_a(void* lpMsg, HWND hWnd, u32 wMsgFilterMin, u32 wMsgFilterMax) {
    return window_manager().get_message(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
}

BOOL Win32ApiHle::hle_peek_message_a(void* lpMsg, HWND hWnd, u32 wMsgFilterMin, u32 wMsgFilterMax, u32 wRemoveMsg) {
    return window_manager().peek_message(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg) ? TRUE_VAL : FALSE_VAL;
}
BOOL Win32ApiHle::hle_dispatch_message_a(const void* lpMsg) {
    window_manager().dispatch_message(lpMsg);
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_translate_message(const void* lpMsg) {
    window_manager().translate_message(lpMsg);
    return TRUE_VAL;
}

// -------------------------------------------------------------
// XINPUT Emulation
// -------------------------------------------------------------
u32 Win32ApiHle::hle_xinput_get_state(u32 dwUserIndex, void* pState) {
    if (!pState || !g_active_input_mgr) return 0x048F;
    input::VirtualGamepadState vpad{};
    if (g_active_input_mgr->get_pad_state(dwUserIndex, vpad)) {
        std::memcpy(pState, &vpad, sizeof(vpad));
        return 0;
    }
    return 0x048F;
}

u32 Win32ApiHle::hle_xinput_set_state(u32 dwUserIndex, void* pVibration) {
    if (!pVibration || !g_active_input_mgr) return 0x048F;
    const auto* vib = static_cast<const input::VirtualVibrationState*>(pVibration);
    g_active_input_mgr->set_vibration(dwUserIndex, vib->left_motor_speed, vib->right_motor_speed);
    return 0;
}

u32 Win32ApiHle::hle_xinput_get_capabilities(u32 dwUserIndex, u32 dwFlags, void* pCapabilities) {
    return 0;
}

// -------------------------------------------------------------
// Steamworks Direct Clean-Room Binding
// -------------------------------------------------------------
BOOL Win32ApiHle::hle_steam_api_init() {
    if (g_active_steam_stub) return g_active_steam_stub->steam_api_init() ? TRUE_VAL : FALSE_VAL;
    return TRUE_VAL;
}

void Win32ApiHle::hle_steam_api_shutdown() {
    if (g_active_steam_stub) g_active_steam_stub->shutdown();
}

void Win32ApiHle::hle_steam_api_run_callbacks() {
    if (g_active_steam_stub) g_active_steam_stub->steam_api_run_callbacks();
}

BOOL Win32ApiHle::hle_steam_api_restart_app_if_necessary(u32 unOwnAppID) {
    return FALSE_VAL;
}

void* Win32ApiHle::hle_steam_internal_create_interface(const char* ver) {
    return nullptr;
}

// -------------------------------------------------------------
// MSVCRT Emulation
// Every host-compiled (mingw/MSVC) game imports these. They are the C runtime
// the CRT startup stub calls before main(): __getmainargs, __set_app_type,
// _initterm, __iob_func, and the raw memory/string/stdio primitives.
// We map them to their libc equivalents so games actually RUN, not crash.
// -------------------------------------------------------------
void* Win32ApiHle::hle_msvcrt_malloc(size_t n)    { return std::malloc(n); }
void  Win32ApiHle::hle_msvcrt_free(void* p)       { if (p) std::free(p); }
void* Win32ApiHle::hle_msvcrt_calloc(size_t a, size_t b) { return std::calloc(a, b); }
void* Win32ApiHle::hle_msvcrt_realloc(void* p, size_t n) { return std::realloc(p, n); }
void* Win32ApiHle::hle_msvcrt_memcpy(void* d, const void* s, size_t n)  { return std::memcpy(d, s, n); }
void* Win32ApiHle::hle_msvcrt_memmove(void* d, const void* s, size_t n) { return std::memmove(d, s, n); }
void* Win32ApiHle::hle_msvcrt_memset(void* d, int c, size_t n)          { return std::memset(d, c, n); }
size_t Win32ApiHle::hle_msvcrt_strlen(const char* s)      { return s ? std::strlen(s) : 0; }
int   Win32ApiHle::hle_msvcrt_strcmp(const char* a, const char* b)      { return std::strcmp(a, b); }
int   Win32ApiHle::hle_msvcrt_strncmp(const char* a, const char* b, size_t n) { return std::strncmp(a, b, n); }
char* Win32ApiHle::hle_msvcrt_strcpy(char* d, const char* s)   { return std::strcpy(d, s); }
char* Win32ApiHle::hle_msvcrt_strncpy(char* d, const char* s, size_t n) { return std::strncpy(d, s, n); }
char* Win32ApiHle::hle_msvcrt_strcat(char* d, const char* s)   { return std::strcat(d, s); }
int   Win32ApiHle::hle_msvcrt_atoi(const char* s)      { return std::atoi(s); }
double Win32ApiHle::hle_msvcrt_atof(const char* s)     { return std::atof(s); }
void* Win32ApiHle::hle_msvcrt_mbstowcs(void* dst, const char* src, size_t n) {
    // Minimal ASCII-only mb->wc: widen in place.
    if (!src) return nullptr;
    for (size_t i = 0; i < n; ++i) {
        reinterpret_cast<wchar_t*>(dst)[i] = static_cast<wchar_t>(
            static_cast<unsigned char>(src[i]));
        if (src[i] == 0) break;
    }
    return dst;
}

// Process lifecycle: exit terminates the guest process (host process exits too,
// matching a real game exiting).
void Win32ApiHle::hle_msvcrt_exit(int code)          { _exit(code); }
void Win32ApiHle::hle_msvcrt__exit(int code)         { _exit(code); }
void Win32ApiHle::hle_msvcrt_abort()                 { abort(); }
void Win32ApiHle::hle_msvcrt__cexit()                { /* flush+return; guest continues */ }
int  Win32ApiHle::hle_msvcrt__initterm(void*, void*) { return 0; } // static-init table walked as no-op
void Win32ApiHle::hle_msvcrt__set_app_type(int)      { /* app type (GUI/console) ignored */ }
void Win32ApiHle::hle_msvcrt__amsg_exit(int)         { /* _amsg_exit prints to stderr on fatal error */ }
void* Win32ApiHle::hle_msvcrt__onexit(void* fn)       { return fn; }
int  Win32ApiHle::hle_msvcrt__ismbblead(u32)         { return 0; }
void Win32ApiHle::hle_msvcrt__setusermatherr(void*)  { /* matherr override ignored */ }
void Win32ApiHle::hle_msvcrt__commode(int)           { /* file translation mode ignored */ }
void Win32ApiHle::hle_msvcrt__fmode(int)             { /* file mode ignored */ }

// __getmainargs(argc, argv, envp, dowildcard, mode): fills the CRT command line
// and environment. The CRT stores the envp result into __initenv and then does
// `mov (%__initenv),%reg; mov envp,(%reg)` — so envp MUST point to a valid,
// non-null empty array, not NULL (NULL would fault on the write-through).
void Win32ApiHle::hle_msvcrt__getmainargs(int* argc, char*** argv, char*** envp, int, int*) {
    // Provide an argc=0 / empty-argv environment. mingw main() tokenizes argv
    // itself; giving a single synthesized argv[0] made its arg-walker deref
    // past the terminator. Empty + non-null is the safe, well-formed default
    // (a real null-terminated env array so __initenv write-through is safe).
    static char* empty_env[1] = { nullptr };
    static char* empty_argv[1] = { nullptr };
    if (argc) *argc = 0;
    if (argv) *argv = empty_argv;
    if (envp) *envp = empty_env;
}

// __iob_func(): the CRT calls this to get the base of the _iob[] array, then
// __acrt_iob_func(i) computes base+i*48. Return our guest _iob slot buffer,
// seeded so slot[0]=stdin, [1]=stdout, [2]=stderr host streams.
void* Win32ApiHle::hle_msvcrt___iob_func() {
    // Host polluted; seed slots only once.
    if (!host_stream(0)) {
        std::memset(g_iob_slots, 0, sizeof(g_iob_slots));
        FILE* h[3] = { stdin, stdout, stderr };
        for (int i = 0; i < 3; ++i) {
            FILE* hf = h[i];
            std::memcpy(g_iob_slots + static_cast<size_t>(i) * kGuestFileStride, &hf, sizeof(hf));
        }
    }
    return g_iob_slots;
}

// ---- stdio ----
int Win32ApiHle::hle_msvcrt_printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap); return r;
}
int Win32ApiHle::hle_msvcrt_fprintf(void* stream, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    FILE* f = host_file_for(stream); if (!f) f = stdout;
    int r = vfprintf(f, fmt, ap);
    va_end(ap); return r;
}
int Win32ApiHle::hle_msvcrt_vfprintf(void* stream, const char* fmt, va_list ap) {
    FILE* f = host_file_for(stream); if (!f) f = stdout;
    return vfprintf(f, fmt, ap);
}
int Win32ApiHle::hle_msvcrt_sprintf(char* buf, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap); return r;
}
size_t Win32ApiHle::hle_msvcrt_fwrite(const void* ptr, size_t sz, size_t n, void* stream) {
    FILE* f = host_file_for(stream); if (!f) f = stdout;
    return fwrite(ptr, sz, n, f);
}
int Win32ApiHle::hle_msvcrt_puts(const char* s)    { return puts(s); }
int Win32ApiHle::hle_msvcrt_fputs(const char* s, void* stream) {
    FILE* f = host_file_for(stream); if (!f) f = stdout;
    return fputs(s, f);
}
int Win32ApiHle::hle_msvcrt_fputc(int c, void* stream) {
    FILE* f = host_file_for(stream); if (!f) f = stdout;
    return fputc(c, f);
}

// ---- misc ----
void* Win32ApiHle::hle_msvcrt_signal(int signum, void* handler) {
    return reinterpret_cast<void*>(signal(signum, reinterpret_cast<void (*)(int)>(handler)));
}
// Structured exception handler dispatcher (x64 only; we never raise SEH, so no-op).
void* Win32ApiHle::hle_msvcrt___C_specific_handler(void* a, void* b, void* c, void* d) {
    return nullptr;
}
int  Win32ApiHle::hle_msvcrt__crt_debugger_hook(int) { return 0; }

// __p__acmdln(): returns &_acmdln so main() can read the command line global.
static PAPAYA_MS_ABI char** hle_msvcrt_p_acmdln() { return &g_acmdln; }

// CRT error accessors: return pointers to per-thread errno variables.
static PAPAYA_MS_ABI int* hle_msvcrt_errno() { return &g_crt_errno; }
static PAPAYA_MS_ABI int** hle_msvcrt_p_errno() {
    g_p_errno_slot = &g_crt_errno;       // __p__errno returns &(the int* holding errno)
    return &g_p_errno_slot;
}
static PAPAYA_MS_ABI unsigned long* hle_msvcrt_doserrno() { return &g_crt_doserrno; }

// ---- KERNEL32 additions ----
void  Win32ApiHle::hle_get_startup_info_a(void* lpStartupInfo) {
    if (!lpStartupInfo) return;
    std::memset(lpStartupInfo, 0, 104); // STARTUPINFOA is 104 bytes
    auto* st = static_cast<u32*>(lpStartupInfo);
    st[0] = 104; // cb
    st[7] = 0;   // dwX/Y etc default
    auto* size_ctx = reinterpret_cast<u16*>(lpStartupInfo);
    size_ctx[0] = 104; // cb (word-safe)
}
void* Win32ApiHle::hle_set_unhandled_exception_filter(void* lpH) {
    (void)lpH; return nullptr;
}
size_t Win32ApiHle::hle_virtual_query(void* lpAddr, int info, void* buf, size_t len) {
    // Minimal MEMORY_BASIC_INFORMATION fill: report RWX private committed.
    if (!buf || len < 48) return 0;
    auto* base = reinterpret_cast<u8*>(buf);
    std::memset(base, 0, 48);
    *reinterpret_cast<void**>(base + 0)  = lpAddr;
    *reinterpret_cast<void**>(base + 8)  = lpAddr;
    *reinterpret_cast<u32*>(base + 16)   = 0x20000; // MEM_PRIVATE
    *reinterpret_cast<u32*>(base + 20)   = 0x1000;  // PAGE_READWRITE
    return 48;
}

// -------------------------------------------------------------
// Semaphore Emulation
// -------------------------------------------------------------
struct NativeSemaphoreState {
    std::mutex mtx;
    std::condition_variable cv;
    s32 count{0};
    s32 max_count{1};
};

HANDLE Win32ApiHle::hle_create_semaphore_a(void* lpSec, s32 lInitialCount, s32 lMaxCount, const char* lpName) {
    auto* sem = new NativeSemaphoreState();
    sem->count     = lInitialCount;
    sem->max_count = (lMaxCount > 0) ? lMaxCount : 0x7FFFFFFF;
    return reinterpret_cast<HANDLE>(sem);
}

HANDLE Win32ApiHle::hle_create_semaphore_w(void* lpSec, s32 lInitialCount, s32 lMaxCount, const wchar_t* lpName) {
    return hle_create_semaphore_a(lpSec, lInitialCount, lMaxCount, nullptr);
}

BOOL Win32ApiHle::hle_release_semaphore(HANDLE hSemaphore, s32 lReleaseCount, s32* lpPreviousCount) {
    if (!hSemaphore) return FALSE_VAL;
    auto* sem = static_cast<NativeSemaphoreState*>(hSemaphore);
    std::lock_guard<std::mutex> lock(sem->mtx);
    if (lpPreviousCount) *lpPreviousCount = sem->count;
    sem->count = std::min(sem->count + lReleaseCount, sem->max_count);
    sem->cv.notify_all();
    return TRUE_VAL;
}

HANDLE Win32ApiHle::hle_open_semaphore_a(u32 dwAccess, BOOL bInherit, const char* lpName) {
    // Named semaphores not tracked; return a fresh default semaphore
    return hle_create_semaphore_a(nullptr, 0, 1, lpName);
}

HANDLE Win32ApiHle::hle_open_event_a(u32 dwAccess, BOOL bInherit, const char* lpName) {
    return hle_create_event_a(nullptr, FALSE_VAL, FALSE_VAL, lpName);
}

HANDLE Win32ApiHle::hle_open_event_w(u32 dwAccess, BOOL bInherit, const wchar_t* lpName) {
    return hle_create_event_a(nullptr, FALSE_VAL, FALSE_VAL, nullptr);
}

HANDLE Win32ApiHle::hle_open_mutex_a(u32 dwAccess, BOOL bInherit, const char* lpName) {
    return hle_create_mutex_a(nullptr, FALSE_VAL, lpName);
}

// -------------------------------------------------------------
// Interlocked Atomic Operations (map to GCC builtins)
// -------------------------------------------------------------
s32 Win32ApiHle::hle_interlocked_increment(volatile s32* lpAddend) {
    return __atomic_add_fetch(lpAddend, 1, __ATOMIC_SEQ_CST);
}

s32 Win32ApiHle::hle_interlocked_decrement(volatile s32* lpAddend) {
    return __atomic_sub_fetch(lpAddend, 1, __ATOMIC_SEQ_CST);
}

s32 Win32ApiHle::hle_interlocked_exchange(volatile s32* Target, s32 Value) {
    s32 old = Value;
    __atomic_exchange(Target, &old, &old, __ATOMIC_SEQ_CST);
    return old;
}

s32 Win32ApiHle::hle_interlocked_compare_exchange(volatile s32* Dest, s32 Exchange, s32 Comparand) {
    s32 expected = Comparand;
    __atomic_compare_exchange(Dest, &expected, &Exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

s64 Win32ApiHle::hle_interlocked_exchange_add(volatile s64* Addend, s64 Value) {
    return __atomic_fetch_add(Addend, Value, __ATOMIC_SEQ_CST);
}

// -------------------------------------------------------------
// Extended Wait
// -------------------------------------------------------------
u32 Win32ApiHle::hle_wait_for_single_object_ex(HANDLE hHandle, u32 dwMilliseconds, BOOL bAlertable) {
    return hle_wait_for_single_object(hHandle, dwMilliseconds);
}

// -------------------------------------------------------------
// Handle Duplication
// -------------------------------------------------------------
BOOL Win32ApiHle::hle_duplicate_handle(HANDLE hSrcProc, HANDLE hSrcHandle, HANDLE hDstProc,
                                        HANDLE* lpTargetHandle, u32 dwAccess, BOOL bInherit, u32 dwOptions) {
    if (lpTargetHandle) *lpTargetHandle = hSrcHandle; // Trivial alias
    return TRUE_VAL;
}

// -------------------------------------------------------------
// Instruction Cache Flush
// -------------------------------------------------------------
BOOL Win32ApiHle::hle_flush_instruction_cache(HANDLE hProcess, const void* lpBaseAddress, size_t dwSize) {
#if defined(__GNUC__) || defined(__clang__)
    if (lpBaseAddress && dwSize > 0) {
        const char* start = static_cast<const char*>(lpBaseAddress);
        __builtin___clear_cache(const_cast<char*>(start), const_cast<char*>(start + dwSize));
    }
#endif
    return TRUE_VAL;
}

// -------------------------------------------------------------
// Console & Debug
// -------------------------------------------------------------
static std::vector<void*> g_ctrl_handlers;

BOOL Win32ApiHle::hle_set_console_ctrl_handler(void* HandlerRoutine, BOOL Add) {
    if (Add && HandlerRoutine) {
        g_ctrl_handlers.push_back(HandlerRoutine);
    } else if (!Add && HandlerRoutine) {
        g_ctrl_handlers.erase(
            std::remove(g_ctrl_handlers.begin(), g_ctrl_handlers.end(), HandlerRoutine),
            g_ctrl_handlers.end());
    }
    return TRUE_VAL;
}

void Win32ApiHle::hle_output_debug_string_a(const char* lpOutputString) {
    if (lpOutputString) {
        log::debug("WIN32_DBG", "{}", lpOutputString);
    }
}

void Win32ApiHle::hle_output_debug_string_w(const wchar_t* lpOutputString) {
    // Convert narrow for logging
    if (lpOutputString) {
        log::debug("WIN32_DBG", "[wide debug string]");
    }
}

BOOL Win32ApiHle::hle_is_debugger_present() {
    return FALSE_VAL; // Always report no debugger to guest
}

// -------------------------------------------------------------
// Wide Char / Multi-byte Conversion
// -------------------------------------------------------------
int Win32ApiHle::hle_multi_byte_to_wide_char(u32 CodePage, u32 dwFlags,
                                              const char* lpMBStr, int cbMB,
                                              wchar_t* lpWStr, int cchWC) {
    if (!lpMBStr) return 0;
    size_t len = (cbMB < 0) ? std::strlen(lpMBStr) + 1 : static_cast<size_t>(cbMB);
    if (cchWC == 0) return static_cast<int>(len);
    size_t out = std::min(static_cast<size_t>(cchWC), len);
    if (lpWStr) {
        for (size_t i = 0; i < out; ++i) {
            lpWStr[i] = static_cast<wchar_t>(static_cast<unsigned char>(lpMBStr[i]));
        }
    }
    return static_cast<int>(out);
}

int Win32ApiHle::hle_wide_char_to_multi_byte(u32 CodePage, u32 dwFlags,
                                              const wchar_t* lpWStr, int cchWC,
                                              char* lpMBStr, int cbMB,
                                              const char* lpDef, BOOL* lpUsed) {
    if (!lpWStr) return 0;
    size_t len = (cchWC < 0) ? (std::wcslen(lpWStr) + 1) : static_cast<size_t>(cchWC);
    if (cbMB == 0) return static_cast<int>(len);
    size_t out = std::min(static_cast<size_t>(cbMB), len);
    if (lpMBStr) {
        for (size_t i = 0; i < out; ++i) {
            lpMBStr[i] = (lpWStr[i] < 0x80) ? static_cast<char>(lpWStr[i]) : '?';
        }
    }
    if (lpUsed) *lpUsed = FALSE_VAL;
    return static_cast<int>(out);
}

// -------------------------------------------------------------
// Locale & Format
// -------------------------------------------------------------
int Win32ApiHle::hle_get_locale_info_a(u32 Locale, u32 LCType, char* lpLCData, int cchData) {
    static const char* locale_str = "en-US";
    if (!lpLCData || cchData == 0) return static_cast<int>(std::strlen(locale_str) + 1);
    std::strncpy(lpLCData, locale_str, static_cast<size_t>(cchData));
    return static_cast<int>(std::strlen(locale_str) + 1);
}

int Win32ApiHle::hle_get_locale_info_w(u32 Locale, u32 LCType, wchar_t* lpLCData, int cchData) {
    static const wchar_t* locale_str = L"en-US";
    if (!lpLCData || cchData == 0) return static_cast<int>(std::wcslen(locale_str) + 1);
    std::wcsncpy(lpLCData, locale_str, static_cast<size_t>(cchData));
    return static_cast<int>(std::wcslen(locale_str) + 1);
}

u32 Win32ApiHle::hle_get_acp() { return 1252; } // Windows-1252 Western European

u32 Win32ApiHle::hle_get_system_default_locale_name(wchar_t* lpLocaleName, int cchLocaleName) {
    static const wchar_t* name = L"en-US";
    if (lpLocaleName && cchLocaleName > 0) {
        std::wcsncpy(lpLocaleName, name, static_cast<size_t>(cchLocaleName));
    }
    return static_cast<u32>(std::wcslen(name));
}

u32 Win32ApiHle::hle_format_message_a(u32 dwFlags, const void* lpSource, u32 dwMsgId,
                                       u32 dwLangId, char* lpBuf, u32 nSize, void* args) {
    if (lpBuf && nSize > 0) {
        std::snprintf(lpBuf, nSize, "Error 0x%08X", dwMsgId);
        return static_cast<u32>(std::strlen(lpBuf));
    }
    return 0;
}

u32 Win32ApiHle::hle_format_message_w(u32 dwFlags, const void* lpSource, u32 dwMsgId,
                                       u32 dwLangId, wchar_t* lpBuf, u32 nSize, void* args) {
    if (lpBuf && nSize > 0) {
        std::swprintf(lpBuf, nSize, L"Error 0x%08X", dwMsgId);
        return static_cast<u32>(std::wcslen(lpBuf));
    }
    return 0;
}

// -------------------------------------------------------------
// Date / Time String Formatting
// -------------------------------------------------------------
// Win32 SYSTEMTIME layout (matches Windows ABI exactly)
struct Win32SystemTime {
    u16 wYear, wMonth, wDayOfWeek, wDay;
    u16 wHour, wMinute, wSecond, wMilliseconds;
};

int Win32ApiHle::hle_get_date_format_a(u32 Locale, u32 dwFlags, const void* lpDate,
                                        const char* lpFormat, char* lpDateStr, int cchDate) {
    if (!lpDateStr || cchDate <= 0) return 8; // "MM/DD/YY" length
    std::strncpy(lpDateStr, "01/01/25", static_cast<size_t>(cchDate));
    return 9;
}

int Win32ApiHle::hle_get_time_format_a(u32 Locale, u32 dwFlags, const void* lpTime,
                                        const char* lpFormat, char* lpTimeStr, int cchTime) {
    if (!lpTimeStr || cchTime <= 0) return 9; // "HH:MM:SS" length
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    if (tm_info) {
        std::strftime(lpTimeStr, static_cast<size_t>(cchTime), "%H:%M:%S", tm_info);
    } else {
        std::strncpy(lpTimeStr, "00:00:00", static_cast<size_t>(cchTime));
    }
    return static_cast<int>(std::strlen(lpTimeStr) + 1);
}

// -------------------------------------------------------------
// System Time
// -------------------------------------------------------------
void Win32ApiHle::hle_get_system_time(void* lpSystemTime) {
    if (!lpSystemTime) return;
    auto* st = static_cast<Win32SystemTime*>(lpSystemTime);
    time_t t = time(nullptr);
    struct tm* utc = gmtime(&t);
    if (!utc) return;
    st->wYear   = static_cast<u16>(utc->tm_year + 1900);
    st->wMonth  = static_cast<u16>(utc->tm_mon + 1);
    st->wDay    = static_cast<u16>(utc->tm_mday);
    st->wHour   = static_cast<u16>(utc->tm_hour);
    st->wMinute = static_cast<u16>(utc->tm_min);
    st->wSecond = static_cast<u16>(utc->tm_sec);
    st->wMilliseconds = 0;
    st->wDayOfWeek = static_cast<u16>(utc->tm_wday);
}

void Win32ApiHle::hle_get_local_time(void* lpSystemTime) {
    if (!lpSystemTime) return;
    auto* st = static_cast<Win32SystemTime*>(lpSystemTime);
    time_t t = time(nullptr);
    struct tm* loc = localtime(&t);
    if (!loc) return;
    st->wYear   = static_cast<u16>(loc->tm_year + 1900);
    st->wMonth  = static_cast<u16>(loc->tm_mon + 1);
    st->wDay    = static_cast<u16>(loc->tm_mday);
    st->wHour   = static_cast<u16>(loc->tm_hour);
    st->wMinute = static_cast<u16>(loc->tm_min);
    st->wSecond = static_cast<u16>(loc->tm_sec);
    st->wMilliseconds = 0;
    st->wDayOfWeek = static_cast<u16>(loc->tm_wday);
}

// Windows FILETIME: 100-nanosecond intervals since Jan 1, 1601
constexpr u64 FILETIME_EPOCH_DIFF = 116444736000000000ULL;

BOOL Win32ApiHle::hle_system_time_to_file_time(const void* lpSystemTime, void* lpFileTime) {
    if (!lpSystemTime || !lpFileTime) return FALSE_VAL;
    const auto* st = static_cast<const Win32SystemTime*>(lpSystemTime);
    struct tm t{};
    t.tm_year = st->wYear - 1900;
    t.tm_mon  = st->wMonth - 1;
    t.tm_mday = st->wDay;
    t.tm_hour = st->wHour;
    t.tm_min  = st->wMinute;
    t.tm_sec  = st->wSecond;
    time_t epoch = timegm(&t);
    u64 ft = static_cast<u64>(epoch) * 10000000ULL + FILETIME_EPOCH_DIFF;
    *static_cast<u64*>(lpFileTime) = ft;
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_file_time_to_system_time(const void* lpFileTime, void* lpSystemTime) {
    if (!lpFileTime || !lpSystemTime) return FALSE_VAL;
    u64 ft = *static_cast<const u64*>(lpFileTime);
    time_t epoch = static_cast<time_t>((ft - FILETIME_EPOCH_DIFF) / 10000000ULL);
    struct tm* utc = gmtime(&epoch);
    if (!utc) return FALSE_VAL;
    auto* st = static_cast<Win32SystemTime*>(lpSystemTime);
    st->wYear   = static_cast<u16>(utc->tm_year + 1900);
    st->wMonth  = static_cast<u16>(utc->tm_mon + 1);
    st->wDay    = static_cast<u16>(utc->tm_mday);
    st->wHour   = static_cast<u16>(utc->tm_hour);
    st->wMinute = static_cast<u16>(utc->tm_min);
    st->wSecond = static_cast<u16>(utc->tm_sec);
    st->wMilliseconds = 0;
    return TRUE_VAL;
}

// -------------------------------------------------------------
// Registry Stubs (Additional)
// -------------------------------------------------------------
s32 Win32ApiHle::hle_reg_open_key_ex_a(void* hKey, const char* lpSubKey, u32 ulOptions, u32 samDesired, void** phkResult) {
    if (phkResult) *phkResult = reinterpret_cast<void*>(0x5000);
    return 0; // ERROR_SUCCESS
}

s32 Win32ApiHle::hle_reg_query_value_ex_a(void* hKey, const char* lpValueName, u32* lpReserved, u32* lpType, u8* lpData, u32* lpcbData) {
    g_last_error = 2; // ERROR_FILE_NOT_FOUND
    return 2;
}

s32 Win32ApiHle::hle_reg_close_key(void* hKey) { return 0; }

s32 Win32ApiHle::hle_reg_set_value_ex_a(void* hKey, const char* lpValueName, u32 Reserved, u32 dwType, const u8* lpData, u32 cbData) {
    return 0; // ERROR_SUCCESS (silently ignore)
}

// -------------------------------------------------------------
// File System Additions
// -------------------------------------------------------------
BOOL Win32ApiHle::hle_create_directory_a(const char* lpPathName, void* lpSec) {
    if (!lpPathName) return FALSE_VAL;
    std::string p = lpPathName;
    std::replace(p.begin(), p.end(), '\\', '/');
    return (mkdir(p.c_str(), 0755) == 0 || errno == EEXIST) ? TRUE_VAL : FALSE_VAL;
}

BOOL Win32ApiHle::hle_remove_directory_a(const char* lpPathName) {
    if (!lpPathName) return FALSE_VAL;
    std::string p = lpPathName;
    std::replace(p.begin(), p.end(), '\\', '/');
    return (rmdir(p.c_str()) == 0) ? TRUE_VAL : FALSE_VAL;
}

BOOL Win32ApiHle::hle_delete_file_a(const char* lpFileName) {
    if (!lpFileName) return FALSE_VAL;
    std::string p = lpFileName;
    std::replace(p.begin(), p.end(), '\\', '/');
    return (unlink(p.c_str()) == 0) ? TRUE_VAL : FALSE_VAL;
}

BOOL Win32ApiHle::hle_copy_file_a(const char* lpExisting, const char* lpNew, BOOL bFailIfExists) {
    if (!lpExisting || !lpNew) return FALSE_VAL;
    std::string src = lpExisting, dst = lpNew;
    std::replace(src.begin(), src.end(), '\\', '/');
    std::replace(dst.begin(), dst.end(), '\\', '/');
    int fd_in  = open(src.c_str(), O_RDONLY);
    int oflags = O_WRONLY | O_CREAT | (bFailIfExists ? O_EXCL : O_TRUNC);
    int fd_out = open(dst.c_str(), oflags, 0666);
    if (fd_in < 0 || fd_out < 0) {
        if (fd_in  >= 0) close(fd_in);
        if (fd_out >= 0) close(fd_out);
        return FALSE_VAL;
    }
    char buf[65536];
    ssize_t n;
    while ((n = read(fd_in, buf, sizeof(buf))) > 0) write(fd_out, buf, static_cast<size_t>(n));
    close(fd_in); close(fd_out);
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_move_file_a(const char* lpExisting, const char* lpNew) {
    if (!lpExisting || !lpNew) return FALSE_VAL;
    std::string src = lpExisting, dst = lpNew;
    std::replace(src.begin(), src.end(), '\\', '/');
    std::replace(dst.begin(), dst.end(), '\\', '/');
    return (rename(src.c_str(), dst.c_str()) == 0) ? TRUE_VAL : FALSE_VAL;
}

u32 Win32ApiHle::hle_get_temp_path_a(u32 nBufferLength, char* lpBuffer) {
    static const char* tmp = "/tmp/";
    if (lpBuffer && nBufferLength > 5) std::strncpy(lpBuffer, tmp, nBufferLength);
    return 5;
}

u32 Win32ApiHle::hle_get_temp_file_name_a(const char* lpPathName, const char* lpPrefixStr, u32 uUnique, char* lpTempFileName) {
    if (!lpTempFileName) return 0;
    const char* dir = lpPathName ? lpPathName : "/tmp";
    const char* pfx = lpPrefixStr ? lpPrefixStr : "tmp";
    std::snprintf(lpTempFileName, 260, "%s/%s%08X.tmp", dir, pfx, uUnique ? uUnique : static_cast<u32>(rand()));
    return uUnique ? uUnique : 1;
}

u32 Win32ApiHle::hle_get_windows_directory_a(char* lpBuffer, u32 uSize) {
    static const char* wd = "C:\\Windows";
    if (lpBuffer && uSize > 10) std::strncpy(lpBuffer, wd, uSize);
    return 10;
}

u32 Win32ApiHle::hle_get_system_directory_a(char* lpBuffer, u32 uSize) {
    static const char* sd = "C:\\Windows\\System32";
    if (lpBuffer && uSize > 19) std::strncpy(lpBuffer, sd, uSize);
    return 19;
}

BOOL Win32ApiHle::hle_get_computer_name_a(char* lpBuffer, u32* lpnSize) {
    static const char* name = "PAPAYA-PC";
    if (lpBuffer && lpnSize && *lpnSize > 9) {
        std::strncpy(lpBuffer, name, *lpnSize);
        *lpnSize = 9;
        return TRUE_VAL;
    }
    return FALSE_VAL;
}

BOOL Win32ApiHle::hle_get_user_name_a(char* lpBuffer, u32* lpnSize) {
    static const char* user = "steamuser";
    if (lpBuffer && lpnSize && *lpnSize > 9) {
        std::strncpy(lpBuffer, user, *lpnSize);
        *lpnSize = 9;
        return TRUE_VAL;
    }
    return FALSE_VAL;
}

void* Win32ApiHle::hle_get_environment_strings() {
    // Return a fake environment block with a double-null terminator
    static char env_block[] = "PAPAYA=1\0";
    return static_cast<void*>(env_block);
}

BOOL Win32ApiHle::hle_free_environment_strings_a(void* lpszEnvironmentBlock) {
    return TRUE_VAL; // Static block; nothing to free
}

u32 Win32ApiHle::hle_set_error_mode(u32 uMode) {
    return uMode; // Return previous mode (same as passed)
}

void Win32ApiHle::hle_raise_exception(u32 code, u32 flags, u32 nargs, const u64* args) {
    log::warn("WIN32", "RaiseException(0x{:08X}, flags=0x{:08X}, nargs={}) — terminating guest process", code, flags, nargs);
    _exit(static_cast<int>(code));
}

// -------------------------------------------------------------
// WINMM (Multimedia & High-Res Timer)
// -------------------------------------------------------------
u32 Win32ApiHle::hle_time_get_time() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

u32 Win32ApiHle::hle_time_begin_period(u32 uPeriod) {
    (void)uPeriod;
    return 0; // TIMERR_NOERROR
}

u32 Win32ApiHle::hle_time_end_period(u32 uPeriod) {
    (void)uPeriod;
    return 0; // TIMERR_NOERROR
}

// -------------------------------------------------------------
// SHELL32
// -------------------------------------------------------------
s32 Win32ApiHle::hle_sh_get_folder_path_a(HWND hwnd, int csidl, HANDLE hToken, u32 dwFlags, char* pszPath) {
    (void)hwnd; (void)csidl; (void)hToken; (void)dwFlags;
    if (!pszPath) return -1;
    std::strncpy(pszPath, "C:\\users\\steamuser\\Documents", 260);
    return 0; // S_OK
}

s32 Win32ApiHle::hle_sh_get_folder_path_w(HWND hwnd, int csidl, HANDLE hToken, u32 dwFlags, wchar_t* pszPath) {
    (void)hwnd; (void)csidl; (void)hToken; (void)dwFlags;
    if (!pszPath) return -1;
    std::wcsncpy(pszPath, L"C:\\users\\steamuser\\Documents", 260);
    return 0; // S_OK
}

s32 Win32ApiHle::hle_sh_get_known_folder_path(const void* rfid, u32 dwFlags, HANDLE hToken, wchar_t** ppszPath) {
    (void)rfid; (void)dwFlags; (void)hToken;
    if (!ppszPath) return -1;
    static wchar_t saved_path[] = L"C:\\users\\steamuser\\Saved Games";
    *ppszPath = saved_path;
    return 0; // S_OK
}

static wchar_t* g_static_argv[] = {
    const_cast<wchar_t*>(L"papaya_game.exe"),
    nullptr
};

wchar_t** Win32ApiHle::hle_command_line_to_argv_w(const wchar_t* lpCmdLine, int* pNumArgs) {
    (void)lpCmdLine;
    if (pNumArgs) *pNumArgs = 1;
    return g_static_argv;
}

// -------------------------------------------------------------
// OLE32 & OLEAUT32 (COM Runtime)
// -------------------------------------------------------------
s32 Win32ApiHle::hle_co_initialize(void* pvReserved) {
    (void)pvReserved;
    return 0; // S_OK
}

s32 Win32ApiHle::hle_co_initialize_ex(void* pvReserved, u32 dwCoInit) {
    (void)pvReserved; (void)dwCoInit;
    return 0; // S_OK
}

void Win32ApiHle::hle_co_uninitialize() {}

s32 Win32ApiHle::hle_co_create_instance(const void* rclsid, void* pUnkOuter, u32 dwClsContext, const void* riid, void** ppv) {
    (void)rclsid; (void)pUnkOuter; (void)dwClsContext; (void)riid;
    if (ppv) *ppv = nullptr;
    return static_cast<s32>(0x80004002); // E_NOINTERFACE
}

void* Win32ApiHle::hle_co_task_mem_alloc(size_t cb) {
    return std::malloc(cb);
}

void Win32ApiHle::hle_co_task_mem_free(void* pv) {
    std::free(pv);
}

// -------------------------------------------------------------
// GDI32 PixelFormat & SwapBuffers
// -------------------------------------------------------------
int Win32ApiHle::hle_choose_pixel_format(void* hdc, const void* ppfd) {
    (void)hdc; (void)ppfd;
    return 1;
}

BOOL Win32ApiHle::hle_set_pixel_format(void* hdc, int format, const void* ppfd) {
    (void)hdc; (void)format; (void)ppfd;
    return TRUE_VAL;
}

int Win32ApiHle::hle_describe_pixel_format(void* hdc, int iPixelFormat, u32 nBytes, void* ppfd) {
    (void)hdc; (void)iPixelFormat; (void)nBytes; (void)ppfd;
    return 1;
}

BOOL Win32ApiHle::hle_swap_buffers(void* hdc) {
    (void)hdc;
    return TRUE_VAL;
}

// -------------------------------------------------------------
// OpenGL & Vulkan Proc Address
// -------------------------------------------------------------
void* Win32ApiHle::hle_wgl_get_proc_address(const char* lpszProc) {
    if (!lpszProc) return nullptr;
    static void* libgl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!libgl) libgl = dlopen("libGL.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!libgl) libgl = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_GLOBAL);
    if (libgl) {
        typedef void* (*glXGetProcAddressARB_fn)(const char*);
        static auto glx_proc = reinterpret_cast<glXGetProcAddressARB_fn>(dlsym(libgl, "glXGetProcAddressARB"));
        if (glx_proc) {
            void* p = glx_proc(lpszProc);
            if (p) return p;
        }
        void* sym = dlsym(libgl, lpszProc);
        if (sym) return sym;
    }
    return reinterpret_cast<void*>(&generic_stub_success);
}

void* Win32ApiHle::hle_vk_get_instance_proc_addr(void* instance, const char* pName) {
    if (!pName) return nullptr;
    static void* libvk = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!libvk) libvk = dlopen("libvulkan.so", RTLD_LAZY | RTLD_GLOBAL);
    if (libvk) {
        typedef void* (*vk_get_proc_fn)(void*, const char*);
        static auto vk_proc = reinterpret_cast<vk_get_proc_fn>(dlsym(libvk, "vkGetInstanceProcAddr"));
        if (vk_proc) {
            return vk_proc(instance, pName);
        }
    }
    return reinterpret_cast<void*>(&generic_stub_success);
}

// -------------------------------------------------------------
// Windows Version & Time
// -------------------------------------------------------------
u32 Win32ApiHle::hle_get_version() {
    return 0x00060002; // Windows 10
}

BOOL Win32ApiHle::hle_get_version_ex_a(void* lpVersionInfo) {
    if (!lpVersionInfo) return FALSE_VAL;
    struct Win32OsVersionInfoA {
        u32 dwOSVersionInfoSize;
        u32 dwMajorVersion;
        u32 dwMinorVersion;
        u32 dwBuildNumber;
        u32 dwPlatformId;
        char szCSDVersion[128];
    };
    auto* vi = static_cast<Win32OsVersionInfoA*>(lpVersionInfo);
    vi->dwMajorVersion = 10;
    vi->dwMinorVersion = 0;
    vi->dwBuildNumber = 19041;
    vi->dwPlatformId = 2; // VER_PLATFORM_WIN32_NT
    std::strncpy(vi->szCSDVersion, "", 128);
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_get_version_ex_w(void* lpVersionInfo) {
    if (!lpVersionInfo) return FALSE_VAL;
    struct Win32OsVersionInfoW {
        u32 dwOSVersionInfoSize;
        u32 dwMajorVersion;
        u32 dwMinorVersion;
        u32 dwBuildNumber;
        u32 dwPlatformId;
        wchar_t szCSDVersion[128];
    };
    auto* vi = static_cast<Win32OsVersionInfoW*>(lpVersionInfo);
    vi->dwMajorVersion = 10;
    vi->dwMinorVersion = 0;
    vi->dwBuildNumber = 19041;
    vi->dwPlatformId = 2;
    std::wcsncpy(vi->szCSDVersion, L"", 128);
    return TRUE_VAL;
}

void Win32ApiHle::hle_get_system_time_as_file_time(void* lpSystemTimeAsFileTime) {
    if (!lpSystemTimeAsFileTime) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    u64 ft = static_cast<u64>(ts.tv_sec) * 10000000ULL + static_cast<u64>(ts.tv_nsec / 100) + FILETIME_EPOCH_DIFF;
    *static_cast<u64*>(lpSystemTimeAsFileTime) = ft;
}

// -------------------------------------------------------------
// Winsock (WS2_32)
// -------------------------------------------------------------
int Win32ApiHle::hle_wsa_startup(u16 wVersionRequested, void* lpWSAData) {
    (void)wVersionRequested; (void)lpWSAData;
    return 0;
}

int Win32ApiHle::hle_wsa_cleanup() {
    return 0;
}

int Win32ApiHle::hle_wsa_get_last_error() {
    return 0;
}

u64 Win32ApiHle::hle_socket(int af, int type, int protocol) {
    int s = ::socket(af, type, protocol);
    return (s >= 0) ? static_cast<u64>(s) : 0xFFFFFFFFFFFFFFFFULL;
}

int Win32ApiHle::hle_closesocket(u64 s) {
    return ::close(static_cast<int>(s));
}

int Win32ApiHle::hle_connect(u64 s, const void* name, int namelen) {
    return ::connect(static_cast<int>(s), static_cast<const struct sockaddr*>(name), static_cast<socklen_t>(namelen));
}

int Win32ApiHle::hle_send(u64 s, const char* buf, int len, int flags) {
    return static_cast<int>(::send(static_cast<int>(s), buf, static_cast<size_t>(len), flags));
}

int Win32ApiHle::hle_recv(u64 s, char* buf, int len, int flags) {
    return static_cast<int>(::recv(static_cast<int>(s), buf, static_cast<size_t>(len), flags));
}

u16 Win32ApiHle::hle_htons(u16 hostshort) {
    return htons(hostshort);
}

u32 Win32ApiHle::hle_htonl(u32 hostlong) {
    return htonl(hostlong);
}

u16 Win32ApiHle::hle_ntohs(u16 netshort) {
    return ntohs(netshort);
}

u32 Win32ApiHle::hle_ntohl(u32 netlong) {
    return ntohl(netlong);
}

// -------------------------------------------------------------
// USER32 Input & Window Additions
// -------------------------------------------------------------
BOOL Win32ApiHle::hle_get_cursor_pos(void* lpPoint) {
    if (!lpPoint) return FALSE_VAL;
    struct Win32Point { s32 x; s32 y; };
    auto* pt = static_cast<Win32Point*>(lpPoint);
    pt->x = 960; pt->y = 540;
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_set_cursor_pos(int X, int Y) {
    (void)X; (void)Y;
    return TRUE_VAL;
}

int Win32ApiHle::hle_show_cursor(BOOL bShow) {
    return bShow ? 1 : 0;
}

s16 Win32ApiHle::hle_get_async_key_state(int vKey) {
    (void)vKey;
    return 0;
}

s16 Win32ApiHle::hle_get_key_state(int vKey) {
    (void)vKey;
    return 0;
}

BOOL Win32ApiHle::hle_get_keyboard_state(u8* lpKeyState) {
    if (lpKeyState) std::memset(lpKeyState, 0, 256);
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_set_window_text_a(HWND hWnd, const char* lpString) {
    (void)hWnd; (void)lpString;
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_set_window_text_w(HWND hWnd, const wchar_t* lpString) {
    (void)hWnd; (void)lpString;
    return TRUE_VAL;
}

int Win32ApiHle::hle_get_window_text_a(HWND hWnd, char* lpString, int nMaxCount) {
    (void)hWnd;
    if (lpString && nMaxCount > 0) {
        std::strncpy(lpString, "Papaya Game", nMaxCount);
        return static_cast<int>(std::strlen(lpString));
    }
    return 0;
}

int Win32ApiHle::hle_get_window_text_w(HWND hWnd, wchar_t* lpString, int nMaxCount) {
    (void)hWnd;
    if (lpString && nMaxCount > 0) {
        std::wcsncpy(lpString, L"Papaya Game", nMaxCount);
        return static_cast<int>(std::wcslen(lpString));
    }
    return 0;
}

BOOL Win32ApiHle::hle_adjust_window_rect(void* lpRect, u32 dwStyle, BOOL bMenu) {
    (void)lpRect; (void)dwStyle; (void)bMenu;
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_adjust_window_rect_ex(void* lpRect, u32 dwStyle, BOOL bMenu, u32 dwExStyle) {
    (void)lpRect; (void)dwStyle; (void)bMenu; (void)dwExStyle;
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_set_window_pos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, u32 uFlags) {
    (void)hWnd; (void)hWndInsertAfter; (void)X; (void)Y; (void)cx; (void)cy; (void)uFlags;
    return TRUE_VAL;
}

// -------------------------------------------------------------
// Win32ApiHle Initialization & Registration Matrix
// -------------------------------------------------------------
Win32ApiHle::Win32ApiHle(
    std::shared_ptr<steam::SteamApiStub> steam_stub,
    std::shared_ptr<input::VirtualXInputManager> input_mgr
) : steam_stub_(steam_stub),
    input_mgr_(input_mgr) {
    g_active_win32_hle = this;
    g_active_steam_stub = steam_stub_;
    g_active_input_mgr = input_mgr_;
}


Result<> Win32ApiHle::initialize() {
    log::info("WIN32", "Registering Comprehensive Win32 & Steam HLE Syscall Dispatch Matrix");

    // KERNEL32.DLL
    register_function("KERNEL32.DLL", "VirtualAlloc", reinterpret_cast<void*>(&hle_virtual_alloc));
    register_function("KERNEL32.DLL", "VirtualFree", reinterpret_cast<void*>(&hle_virtual_free));
    register_function("KERNEL32.DLL", "VirtualProtect", reinterpret_cast<void*>(&hle_virtual_protect));
    register_function("KERNEL32.DLL", "GetProcessHeap", reinterpret_cast<void*>(&hle_get_process_heap));
    register_function("KERNEL32.DLL", "HeapAlloc", reinterpret_cast<void*>(&hle_heap_alloc));
    register_function("KERNEL32.DLL", "HeapFree", reinterpret_cast<void*>(&hle_heap_free));
    register_function("KERNEL32.DLL", "HeapReAlloc", reinterpret_cast<void*>(&hle_heap_realloc));
    register_function("KERNEL32.DLL", "LocalAlloc", reinterpret_cast<void*>(&hle_local_alloc));
    register_function("KERNEL32.DLL", "LocalFree", reinterpret_cast<void*>(&hle_local_free));

    register_function("KERNEL32.DLL", "TlsAlloc", reinterpret_cast<void*>(&hle_tls_alloc));
    register_function("KERNEL32.DLL", "TlsFree", reinterpret_cast<void*>(&hle_tls_free));
    register_function("KERNEL32.DLL", "TlsGetValue", reinterpret_cast<void*>(&hle_tls_get_value));
    register_function("KERNEL32.DLL", "TlsSetValue", reinterpret_cast<void*>(&hle_tls_set_value));

    register_function("KERNEL32.DLL", "CreateThread", reinterpret_cast<void*>(&hle_create_thread));
    register_function("KERNEL32.DLL", "GetCurrentThreadId", reinterpret_cast<void*>(&hle_get_current_thread_id));
    register_function("KERNEL32.DLL", "GetCurrentProcessId", reinterpret_cast<void*>(&hle_get_current_process_id));
    register_function("KERNEL32.DLL", "GetCurrentThread", reinterpret_cast<void*>(&hle_get_current_thread));
    register_function("KERNEL32.DLL", "GetCurrentProcess", reinterpret_cast<void*>(&hle_get_current_process));
    register_function("KERNEL32.DLL", "ExitProcess", reinterpret_cast<void*>(&hle_exit_process));
    register_function("KERNEL32.DLL", "ExitThread", reinterpret_cast<void*>(&hle_exit_thread));
    register_function("KERNEL32.DLL", "Sleep", reinterpret_cast<void*>(&hle_sleep));
    register_function("KERNEL32.DLL", "SleepEx", reinterpret_cast<void*>(&hle_sleep_ex));
    register_function("KERNEL32.DLL", "SwitchToThread", reinterpret_cast<void*>(&hle_switch_to_thread));

    register_function("KERNEL32.DLL", "InitializeCriticalSection", reinterpret_cast<void*>(&hle_init_critical_section));
    register_function("KERNEL32.DLL", "InitializeCriticalSectionAndSpinCount", reinterpret_cast<void*>(&hle_init_critical_section_and_spin_count));
    register_function("KERNEL32.DLL", "EnterCriticalSection", reinterpret_cast<void*>(&hle_enter_critical_section));
    register_function("KERNEL32.DLL", "TryEnterCriticalSection", reinterpret_cast<void*>(&hle_try_enter_critical_section));
    register_function("KERNEL32.DLL", "LeaveCriticalSection", reinterpret_cast<void*>(&hle_leave_critical_section));
    register_function("KERNEL32.DLL", "DeleteCriticalSection", reinterpret_cast<void*>(&hle_delete_critical_section));

    register_function("KERNEL32.DLL", "CreateEventA", reinterpret_cast<void*>(&hle_create_event_a));
    register_function("KERNEL32.DLL", "CreateEventW", reinterpret_cast<void*>(&hle_create_event_w));
    register_function("KERNEL32.DLL", "SetEvent", reinterpret_cast<void*>(&hle_set_event));
    register_function("KERNEL32.DLL", "ResetEvent", reinterpret_cast<void*>(&hle_reset_event));
    register_function("KERNEL32.DLL", "CreateMutexA", reinterpret_cast<void*>(&hle_create_mutex_a));
    register_function("KERNEL32.DLL", "ReleaseMutex", reinterpret_cast<void*>(&hle_release_mutex));
    register_function("KERNEL32.DLL", "WaitForSingleObject", reinterpret_cast<void*>(&hle_wait_for_single_object));
    register_function("KERNEL32.DLL", "WaitForMultipleObjects", reinterpret_cast<void*>(&hle_wait_for_multiple_objects));

    register_function("KERNEL32.DLL", "CreateFileA", reinterpret_cast<void*>(&hle_create_file_a));
    register_function("KERNEL32.DLL", "CreateFileW", reinterpret_cast<void*>(&hle_create_file_w));
    register_function("KERNEL32.DLL", "ReadFile", reinterpret_cast<void*>(&hle_read_file));
    register_function("KERNEL32.DLL", "WriteFile", reinterpret_cast<void*>(&hle_write_file));
    register_function("KERNEL32.DLL", "CloseHandle", reinterpret_cast<void*>(&hle_close_handle));
    register_function("KERNEL32.DLL", "GetFileSize", reinterpret_cast<void*>(&hle_get_file_size));
    register_function("KERNEL32.DLL", "SetFilePointer", reinterpret_cast<void*>(&hle_set_file_pointer));
    register_function("KERNEL32.DLL", "GetFileAttributesA", reinterpret_cast<void*>(&hle_get_file_attributes_a));
    register_function("KERNEL32.DLL", "GetFileAttributesW", reinterpret_cast<void*>(&hle_get_file_attributes_w));
    register_function("KERNEL32.DLL", "GetFullPathNameA", reinterpret_cast<void*>(&hle_get_full_path_name_a));
    register_function("KERNEL32.DLL", "GetCurrentDirectoryA", reinterpret_cast<void*>(&hle_get_current_directory_a));
    register_function("KERNEL32.DLL", "SetCurrentDirectoryA", reinterpret_cast<void*>(&hle_set_current_directory_a));
    register_function("KERNEL32.DLL", "FindFirstFileA", reinterpret_cast<void*>(&hle_find_first_file_a));
    register_function("KERNEL32.DLL", "FindNextFileA", reinterpret_cast<void*>(&hle_find_next_file_a));
    register_function("KERNEL32.DLL", "FindClose", reinterpret_cast<void*>(&hle_find_close));

    register_function("KERNEL32.DLL", "GetProcAddress", reinterpret_cast<void*>(&hle_get_proc_address));
    register_function("KERNEL32.DLL", "GetModuleHandleA", reinterpret_cast<void*>(&hle_get_module_handle_a));
    register_function("KERNEL32.DLL", "GetModuleHandleW", reinterpret_cast<void*>(&hle_get_module_handle_w));
    register_function("KERNEL32.DLL", "LoadLibraryA", reinterpret_cast<void*>(&hle_load_library_a));
    register_function("KERNEL32.DLL", "LoadLibraryW", reinterpret_cast<void*>(&hle_load_library_w));
    register_function("KERNEL32.DLL", "FreeLibrary", reinterpret_cast<void*>(&hle_free_library));
    register_function("KERNEL32.DLL", "GetModuleFileNameA", reinterpret_cast<void*>(&hle_get_module_file_name_a));

    register_function("KERNEL32.DLL", "GetSystemInfo", reinterpret_cast<void*>(&hle_get_system_info));
    register_function("KERNEL32.DLL", "GetNativeSystemInfo", reinterpret_cast<void*>(&hle_get_native_system_info));
    register_function("KERNEL32.DLL", "IsProcessorFeaturePresent", reinterpret_cast<void*>(&hle_is_processor_feature_present));
    register_function("KERNEL32.DLL", "GetCommandLineA", reinterpret_cast<void*>(&hle_get_command_line_a));
    register_function("KERNEL32.DLL", "GetCommandLineW", reinterpret_cast<void*>(&hle_get_command_line_w));
    register_function("KERNEL32.DLL", "GetEnvironmentVariableA", reinterpret_cast<void*>(&hle_get_environment_variable_a));
    register_function("KERNEL32.DLL", "QueryPerformanceCounter", reinterpret_cast<void*>(&hle_query_performance_counter));
    register_function("KERNEL32.DLL", "QueryPerformanceFrequency", reinterpret_cast<void*>(&hle_query_performance_frequency));
    register_function("KERNEL32.DLL", "GetTickCount", reinterpret_cast<void*>(&hle_get_tick_count));
    register_function("KERNEL32.DLL", "GetTickCount64", reinterpret_cast<void*>(&hle_get_tick_count_64));
    register_function("KERNEL32.DLL", "GetLastError", reinterpret_cast<void*>(&hle_get_last_error));
    register_function("KERNEL32.DLL", "SetLastError", reinterpret_cast<void*>(&hle_set_last_error));
    register_function("KERNEL32.DLL", "GetStartupInfoA", reinterpret_cast<void*>(&hle_get_startup_info_a));
    register_function("KERNEL32.DLL", "SetUnhandledExceptionFilter", reinterpret_cast<void*>(&hle_set_unhandled_exception_filter));
    register_function("KERNEL32.DLL", "VirtualQuery", reinterpret_cast<void*>(&hle_virtual_query));

    // NTDLL.DLL
    register_function("NTDLL.DLL", "NtAllocateVirtualMemory", reinterpret_cast<void*>(&hle_virtual_alloc));
    register_function("NTDLL.DLL", "NtFreeVirtualMemory", reinterpret_cast<void*>(&hle_virtual_free));
    register_function("NTDLL.DLL", "RtlInitializeCriticalSection", reinterpret_cast<void*>(&hle_init_critical_section));
    register_function("NTDLL.DLL", "RtlEnterCriticalSection", reinterpret_cast<void*>(&hle_enter_critical_section));
    register_function("NTDLL.DLL", "RtlLeaveCriticalSection", reinterpret_cast<void*>(&hle_leave_critical_section));
    register_function("NTDLL.DLL", "RtlDeleteCriticalSection", reinterpret_cast<void*>(&hle_delete_critical_section));

    // MSVCRT.DLL - the C runtime every mingw/MSVC binary imports.
    register_function("msvcrt.dll", "malloc",   reinterpret_cast<void*>(&hle_msvcrt_malloc));
    register_function("msvcrt.dll", "free",     reinterpret_cast<void*>(&hle_msvcrt_free));
    register_function("msvcrt.dll", "calloc",   reinterpret_cast<void*>(&hle_msvcrt_calloc));
    register_function("msvcrt.dll", "realloc",  reinterpret_cast<void*>(&hle_msvcrt_realloc));
    register_function("msvcrt.dll", "memcpy",   reinterpret_cast<void*>(&hle_msvcrt_memcpy));
    register_function("msvcrt.dll", "memmove",  reinterpret_cast<void*>(&hle_msvcrt_memmove));
    register_function("msvcrt.dll", "memset",   reinterpret_cast<void*>(&hle_msvcrt_memset));
    register_function("msvcrt.dll", "strlen",   reinterpret_cast<void*>(&hle_msvcrt_strlen));
    register_function("msvcrt.dll", "strcmp",   reinterpret_cast<void*>(&hle_msvcrt_strcmp));
    register_function("msvcrt.dll", "strncmp",  reinterpret_cast<void*>(&hle_msvcrt_strncmp));
    register_function("msvcrt.dll", "strcpy",   reinterpret_cast<void*>(&hle_msvcrt_strcpy));
    register_function("msvcrt.dll", "strncpy",  reinterpret_cast<void*>(&hle_msvcrt_strncpy));
    register_function("msvcrt.dll", "strcat",   reinterpret_cast<void*>(&hle_msvcrt_strcat));
    register_function("msvcrt.dll", "atoi",     reinterpret_cast<void*>(&hle_msvcrt_atoi));
    register_function("msvcrt.dll", "atof",     reinterpret_cast<void*>(&hle_msvcrt_atof));
    register_function("msvcrt.dll", "mbstowcs", reinterpret_cast<void*>(&hle_msvcrt_mbstowcs));

    register_function("msvcrt.dll", "exit",     reinterpret_cast<void*>(&hle_msvcrt_exit));
    register_function("msvcrt.dll", "_exit",    reinterpret_cast<void*>(&hle_msvcrt__exit));
    register_function("msvcrt.dll", "abort",    reinterpret_cast<void*>(&hle_msvcrt_abort));
    register_function("msvcrt.dll", "_cexit",   reinterpret_cast<void*>(&hle_msvcrt__cexit));
    register_function("msvcrt.dll", "_initterm", reinterpret_cast<void*>(&hle_msvcrt__initterm));
    register_function("msvcrt.dll", "__set_app_type", reinterpret_cast<void*>(&hle_msvcrt__set_app_type));
    register_function("msvcrt.dll", "_set_app_type",  reinterpret_cast<void*>(&hle_msvcrt__set_app_type));
    register_function("msvcrt.dll", "_amsg_exit", reinterpret_cast<void*>(&hle_msvcrt__amsg_exit));
    register_function("msvcrt.dll", "_onexit",  reinterpret_cast<void*>(&hle_msvcrt__onexit));
    register_function("msvcrt.dll", "_ismbblead", reinterpret_cast<void*>(&hle_msvcrt__ismbblead));
    register_function("msvcrt.dll", "__getmainargs", reinterpret_cast<void*>(&hle_msvcrt__getmainargs));
    register_function("msvcrt.dll", "__setusermatherr", reinterpret_cast<void*>(&hle_msvcrt__setusermatherr));
    register_function("msvcrt.dll", "_commode", reinterpret_cast<void*>(&hle_msvcrt__commode));
    register_function("msvcrt.dll", "_fmode",   reinterpret_cast<void*>(&hle_msvcrt__fmode));

    register_function("msvcrt.dll", "printf",   reinterpret_cast<void*>(&hle_msvcrt_printf));
    register_function("msvcrt.dll", "fprintf",  reinterpret_cast<void*>(&hle_msvcrt_fprintf));
    register_function("msvcrt.dll", "vfprintf", reinterpret_cast<void*>(&hle_msvcrt_vfprintf));
    register_function("msvcrt.dll", "sprintf",  reinterpret_cast<void*>(&hle_msvcrt_sprintf));
    register_function("msvcrt.dll", "fwrite",   reinterpret_cast<void*>(&hle_msvcrt_fwrite));
    register_function("msvcrt.dll", "puts",     reinterpret_cast<void*>(&hle_msvcrt_puts));
    register_function("msvcrt.dll", "fputs",    reinterpret_cast<void*>(&hle_msvcrt_fputs));
    register_function("msvcrt.dll", "fputc",    reinterpret_cast<void*>(&hle_msvcrt_fputc));
    register_function("msvcrt.dll", "__iob_func", reinterpret_cast<void*>(&hle_msvcrt___iob_func));

    register_function("msvcrt.dll", "signal",   reinterpret_cast<void*>(&hle_msvcrt_signal));
    register_function("msvcrt.dll", "__C_specific_handler", reinterpret_cast<void*>(&hle_msvcrt___C_specific_handler));
    register_function("msvcrt.dll", "_crt_debugger_hook", reinterpret_cast<void*>(&hle_msvcrt__crt_debugger_hook));

    // msvcrt data imports (resolved to real writable variable addresses, not stubs).
    register_function("msvcrt.dll", "__initenv", reinterpret_cast<void*>(&g_initenv));
    register_function("msvcrt.dll", "_environ",  reinterpret_cast<void*>(&g_environ));
    register_function("msvcrt.dll", "__envp",    reinterpret_cast<void*>(&g_environ));
    register_function("msvcrt.dll", "_acmdln",   reinterpret_cast<void*>(&g_acmdln));
    register_function("msvcrt.dll", "__p__acmdln", reinterpret_cast<void*>(&hle_msvcrt_p_acmdln));
    register_function("msvcrt.dll", "_errno",    reinterpret_cast<void*>(&hle_msvcrt_errno));
    register_function("msvcrt.dll", "__p__errno", reinterpret_cast<void*>(&hle_msvcrt_p_errno));
    register_function("msvcrt.dll", "_doserrno", reinterpret_cast<void*>(&hle_msvcrt_doserrno));
    register_function("msvcrt.dll", "__p__doserrno", reinterpret_cast<void*>(&hle_msvcrt_doserrno));

    // USER32.DLL & GDI32.DLL
    register_function("USER32.DLL", "GetSystemMetrics", reinterpret_cast<void*>(&hle_get_system_metrics));
    register_function("USER32.DLL", "SetProcessDPIAware", reinterpret_cast<void*>(&hle_set_process_dpi_aware));
    register_function("USER32.DLL", "GetClientRect", reinterpret_cast<void*>(&hle_get_client_rect));
    register_function("USER32.DLL", "GetWindowRect", reinterpret_cast<void*>(&hle_get_window_rect));
    register_function("USER32.DLL", "PeekMessageA", reinterpret_cast<void*>(&hle_peek_message_a));
    register_function("USER32.DLL", "GetMessageA", reinterpret_cast<void*>(&hle_get_message_a));
    register_function("USER32.DLL", "DispatchMessageA", reinterpret_cast<void*>(&hle_dispatch_message_a));
    register_function("USER32.DLL", "TranslateMessage", reinterpret_cast<void*>(&hle_translate_message));
    register_function("USER32.DLL", "RegisterClassA", reinterpret_cast<void*>(&hle_register_class_a));
    register_function("USER32.DLL", "RegisterClassExA", reinterpret_cast<void*>(&hle_register_class_a));
    register_function("USER32.DLL", "CreateWindowExA", reinterpret_cast<void*>(&hle_create_window_ex_a));
    register_function("USER32.DLL", "CreateWindowA", reinterpret_cast<void*>(&hle_create_window_ex_a));
    register_function("USER32.DLL", "DestroyWindow", reinterpret_cast<void*>(&hle_destroy_window));
    register_function("USER32.DLL", "ShowWindow", reinterpret_cast<void*>(&hle_show_window));
    register_function("USER32.DLL", "UpdateWindow", reinterpret_cast<void*>(&hle_update_window));
    register_function("USER32.DLL", "DefWindowProcA", reinterpret_cast<void*>(&hle_def_window_proc_a));
    register_function("USER32.DLL", "PostQuitMessage", reinterpret_cast<void*>(&hle_post_quit_message));
    register_function("USER32.DLL", "PostMessageA", reinterpret_cast<void*>(&hle_post_message_a));
    register_function("USER32.DLL", "SendMessageA", reinterpret_cast<void*>(&hle_send_message_a));
    register_function("USER32.DLL", "GetDC", reinterpret_cast<void*>(&hle_get_dc));
    register_function("USER32.DLL", "ReleaseDC", reinterpret_cast<void*>(&hle_release_dc));
    register_function("USER32.DLL", "BeginPaint", reinterpret_cast<void*>(&generic_stub_null));
    register_function("USER32.DLL", "EndPaint", reinterpret_cast<void*>(&generic_stub_success));
    register_function("USER32.DLL", "InvalidateRect", reinterpret_cast<void*>(&generic_stub_success));
    register_function("USER32.DLL", "MessageBoxA", reinterpret_cast<void*>(&generic_stub_zero));

    register_function("GDI32.DLL", "SelectObject", reinterpret_cast<void*>(&generic_stub_success));
    register_function("GDI32.DLL", "GetPixel", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("GDI32.DLL", "GetDIBits", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("GDI32.DLL", "GetDeviceCaps", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("GDI32.DLL", "DeleteObject", reinterpret_cast<void*>(&generic_stub_success));
    register_function("GDI32.DLL", "DeleteDC", reinterpret_cast<void*>(&generic_stub_success));

    // AVRT.DLL & DWMAPI.DLL
    register_function("AVRT.DLL", "AvSetMmThreadCharacteristicsW", reinterpret_cast<void*>(&generic_stub_success));
    register_function("AVRT.DLL", "AvSetMmThreadPriority", reinterpret_cast<void*>(&generic_stub_success));
    register_function("dwmapi.dll", "DwmEnableBlurBehindWindow", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("dwmapi.dll", "DwmSetWindowAttribute", reinterpret_cast<void*>(&generic_stub_zero));

    // BCRYPT.DLL & CRYPT32.DLL
    register_function("bcrypt.dll", "BCryptGenRandom", reinterpret_cast<void*>(&hle_bcrypt_gen_random));
    register_function("CRYPT32.dll", "CertOpenSystemStoreA", reinterpret_cast<void*>(&generic_stub_null));
    register_function("CRYPT32.dll", "CertCloseStore", reinterpret_cast<void*>(&generic_stub_success));
    register_function("CRYPT32.dll", "CertGetCertificateContextProperty", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("CRYPT32.dll", "CryptBinaryToStringA", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("CRYPT32.dll", "CertEnumCertificatesInStore", reinterpret_cast<void*>(&generic_stub_null));

    // ADVAPI32.DLL & SHELL32.DLL
    register_function("ADVAPI32.dll", "OpenProcessToken", reinterpret_cast<void*>(&generic_stub_success));
    register_function("ADVAPI32.dll", "GetTokenInformation", reinterpret_cast<void*>(&generic_stub_success));
    register_function("ADVAPI32.dll", "RegOpenKeyExW", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("ADVAPI32.dll", "RegQueryValueExW", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("ADVAPI32.dll", "RegCloseKey", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("ADVAPI32.dll", "RegGetValueW", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("ADVAPI32.dll", "RegEnumValueW", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("ADVAPI32.dll", "GetCurrentHwProfileA", reinterpret_cast<void*>(&generic_stub_success));
    register_function("ADVAPI32.dll", "LookupPrivilegeValueW", reinterpret_cast<void*>(&generic_stub_success));
    register_function("ADVAPI32.dll", "AdjustTokenPrivileges", reinterpret_cast<void*>(&generic_stub_success));
    register_function("ADVAPI32.dll", "GetSidSubAuthority", reinterpret_cast<void*>(&generic_stub_null));
    register_function("ADVAPI32.dll", "GetSidSubAuthorityCount", reinterpret_cast<void*>(&generic_stub_null));

    register_function("SHELL32.dll", "ShellExecuteW", reinterpret_cast<void*>(&generic_stub_success));
    register_function("SHELL32.dll", "CommandLineToArgvW", reinterpret_cast<void*>(&generic_stub_null));
    register_function("SHELL32.dll", "SHFileOperationW", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("SHELL32.dll", "SHGetKnownFolderPath", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("SHELL32.dll", "DragAcceptFiles", reinterpret_cast<void*>(&generic_stub_null));
    register_function("SHELL32.dll", "DragQueryFileW", reinterpret_cast<void*>(&generic_stub_zero));

    // IMM32.DLL
    register_function("IMM32.dll", "ImmGetContext", reinterpret_cast<void*>(&generic_stub_null));
    register_function("IMM32.dll", "ImmReleaseContext", reinterpret_cast<void*>(&generic_stub_success));
    register_function("IMM32.dll", "ImmSetCandidateWindow", reinterpret_cast<void*>(&generic_stub_success));
    register_function("IMM32.dll", "ImmGetCompositionStringW", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("IMM32.dll", "ImmSetCompositionWindow", reinterpret_cast<void*>(&generic_stub_success));
    register_function("IMM32.dll", "ImmAssociateContext", reinterpret_cast<void*>(&generic_stub_null));

    // OPENGL32.DLL
    register_function("OPENGL32.dll", "wglCreateContext", reinterpret_cast<void*>(&generic_stub_null));
    register_function("OPENGL32.dll", "wglDeleteContext", reinterpret_cast<void*>(&generic_stub_success));
    register_function("OPENGL32.dll", "wglGetProcAddress", reinterpret_cast<void*>(&generic_stub_null));
    register_function("OPENGL32.dll", "wglMakeCurrent", reinterpret_cast<void*>(&generic_stub_success));

    // WS2_32.DLL / WSOCK32.DLL
    register_function("WS2_32.dll", "WSAConnect", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("WS2_32.dll", "getaddrinfo", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("WS2_32.dll", "freeaddrinfo", reinterpret_cast<void*>(&generic_stub_null));
    register_function("WS2_32.dll", "getnameinfo", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("WS2_32.dll", "inet_pton", reinterpret_cast<void*>(&generic_stub_success));

    // XINPUT1_4.DLL / XINPUT9_1_0.DLL
    register_function("XINPUT1_4.DLL", "XInputGetState", reinterpret_cast<void*>(&hle_xinput_get_state));
    register_function("XINPUT1_4.DLL", "XInputSetState", reinterpret_cast<void*>(&hle_xinput_set_state));
    register_function("XINPUT1_4.DLL", "XInputGetCapabilities", reinterpret_cast<void*>(&hle_xinput_get_capabilities));
    register_function("XINPUT9_1_0.DLL", "XInputGetState", reinterpret_cast<void*>(&hle_xinput_get_state));
    register_function("XINPUT9_1_0.DLL", "XInputSetState", reinterpret_cast<void*>(&hle_xinput_set_state));

    // STEAM_API64.DLL / STEAM_API.DLL
    register_function("STEAM_API64.DLL", "SteamAPI_Init", reinterpret_cast<void*>(&hle_steam_api_init));
    register_function("STEAM_API64.DLL", "SteamAPI_Shutdown", reinterpret_cast<void*>(&hle_steam_api_shutdown));
    register_function("STEAM_API64.DLL", "SteamAPI_RunCallbacks", reinterpret_cast<void*>(&hle_steam_api_run_callbacks));
    register_function("STEAM_API64.DLL", "SteamAPI_RestartAppIfNecessary", reinterpret_cast<void*>(&hle_steam_api_restart_app_if_necessary));
    register_function("STEAM_API64.DLL", "SteamInternal_CreateInterface", reinterpret_cast<void*>(&hle_steam_internal_create_interface));
    register_function("STEAM_API64.DLL", "SteamInternal_SteamAPI_Init", reinterpret_cast<void*>(&hle_steam_api_init));
    register_function("STEAM_API64.DLL", "SteamInternal_ContextInit", reinterpret_cast<void*>(&generic_stub_success));
    register_function("STEAM_API64.DLL", "SteamInternal_FindOrCreateUserInterface", reinterpret_cast<void*>(&generic_stub_null));
    register_function("STEAM_API64.DLL", "SteamInternal_FindOrCreateGameServerInterface", reinterpret_cast<void*>(&generic_stub_null));
    register_function("STEAM_API64.DLL", "SteamAPI_RegisterCallback", reinterpret_cast<void*>(&generic_stub_null));
    register_function("STEAM_API64.DLL", "SteamAPI_UnregisterCallback", reinterpret_cast<void*>(&generic_stub_null));
    register_function("STEAM_API64.DLL", "SteamAPI_RegisterCallResult", reinterpret_cast<void*>(&generic_stub_null));
    register_function("STEAM_API64.DLL", "SteamAPI_UnregisterCallResult", reinterpret_cast<void*>(&generic_stub_null));
    register_function("STEAM_API64.DLL", "SteamAPI_IsSteamRunning", reinterpret_cast<void*>(&generic_stub_success));
    register_function("STEAM_API64.DLL", "SteamAPI_GetHSteamUser", reinterpret_cast<void*>(&generic_stub_success));
    register_function("STEAM_API64.DLL", "SteamGameServer_GetHSteamUser", reinterpret_cast<void*>(&generic_stub_success));

    register_function("STEAM_API.DLL", "SteamAPI_Init", reinterpret_cast<void*>(&hle_steam_api_init));
    register_function("STEAM_API.DLL", "SteamAPI_Shutdown", reinterpret_cast<void*>(&hle_steam_api_shutdown));
    register_function("STEAM_API.DLL", "SteamAPI_RunCallbacks", reinterpret_cast<void*>(&hle_steam_api_run_callbacks));
    register_function("STEAM_API.DLL", "SteamAPI_RestartAppIfNecessary", reinterpret_cast<void*>(&hle_steam_api_restart_app_if_necessary));

    // ---- Extended KERNEL32 ----

    // Semaphore
    register_function("KERNEL32.DLL", "CreateSemaphoreA", reinterpret_cast<void*>(&hle_create_semaphore_a));
    register_function("KERNEL32.DLL", "CreateSemaphoreW", reinterpret_cast<void*>(&hle_create_semaphore_w));
    register_function("KERNEL32.DLL", "ReleaseSemaphore", reinterpret_cast<void*>(&hle_release_semaphore));
    register_function("KERNEL32.DLL", "OpenSemaphoreA",   reinterpret_cast<void*>(&hle_open_semaphore_a));
    register_function("KERNEL32.DLL", "OpenEventA",       reinterpret_cast<void*>(&hle_open_event_a));
    register_function("KERNEL32.DLL", "OpenEventW",       reinterpret_cast<void*>(&hle_open_event_w));
    register_function("KERNEL32.DLL", "OpenMutexA",       reinterpret_cast<void*>(&hle_open_mutex_a));

    // Interlocked atomics
    register_function("KERNEL32.DLL", "InterlockedIncrement",      reinterpret_cast<void*>(&hle_interlocked_increment));
    register_function("KERNEL32.DLL", "InterlockedDecrement",      reinterpret_cast<void*>(&hle_interlocked_decrement));
    register_function("KERNEL32.DLL", "InterlockedExchange",       reinterpret_cast<void*>(&hle_interlocked_exchange));
    register_function("KERNEL32.DLL", "InterlockedCompareExchange",reinterpret_cast<void*>(&hle_interlocked_compare_exchange));
    register_function("KERNEL32.DLL", "InterlockedExchangeAdd64",  reinterpret_cast<void*>(&hle_interlocked_exchange_add));

    // Extended wait
    register_function("KERNEL32.DLL", "WaitForSingleObjectEx",    reinterpret_cast<void*>(&hle_wait_for_single_object_ex));

    // Handle duplication
    register_function("KERNEL32.DLL", "DuplicateHandle", reinterpret_cast<void*>(&hle_duplicate_handle));

    // Instruction cache flush
    register_function("KERNEL32.DLL", "FlushInstructionCache", reinterpret_cast<void*>(&hle_flush_instruction_cache));

    // Console & debug
    register_function("KERNEL32.DLL", "SetConsoleCtrlHandler",  reinterpret_cast<void*>(&hle_set_console_ctrl_handler));
    register_function("KERNEL32.DLL", "OutputDebugStringA",     reinterpret_cast<void*>(&hle_output_debug_string_a));
    register_function("KERNEL32.DLL", "OutputDebugStringW",     reinterpret_cast<void*>(&hle_output_debug_string_w));
    register_function("KERNEL32.DLL", "IsDebuggerPresent",      reinterpret_cast<void*>(&hle_is_debugger_present));

    // Wide char conversion
    register_function("KERNEL32.DLL", "MultiByteToWideChar", reinterpret_cast<void*>(&hle_multi_byte_to_wide_char));
    register_function("KERNEL32.DLL", "WideCharToMultiByte", reinterpret_cast<void*>(&hle_wide_char_to_multi_byte));

    // Locale & format
    register_function("KERNEL32.DLL", "GetLocaleInfoA",          reinterpret_cast<void*>(&hle_get_locale_info_a));
    register_function("KERNEL32.DLL", "GetLocaleInfoW",          reinterpret_cast<void*>(&hle_get_locale_info_w));
    register_function("KERNEL32.DLL", "GetACP",                  reinterpret_cast<void*>(&hle_get_acp));
    register_function("KERNEL32.DLL", "GetSystemDefaultLocaleName", reinterpret_cast<void*>(&hle_get_system_default_locale_name));
    register_function("KERNEL32.DLL", "FormatMessageA",          reinterpret_cast<void*>(&hle_format_message_a));
    register_function("KERNEL32.DLL", "FormatMessageW",          reinterpret_cast<void*>(&hle_format_message_w));

    // Date/time
    register_function("KERNEL32.DLL", "GetDateFormatA",   reinterpret_cast<void*>(&hle_get_date_format_a));
    register_function("KERNEL32.DLL", "GetTimeFormatA",   reinterpret_cast<void*>(&hle_get_time_format_a));
    register_function("KERNEL32.DLL", "GetSystemTime",    reinterpret_cast<void*>(&hle_get_system_time));
    register_function("KERNEL32.DLL", "GetLocalTime",     reinterpret_cast<void*>(&hle_get_local_time));
    register_function("KERNEL32.DLL", "SystemTimeToFileTime", reinterpret_cast<void*>(&hle_system_time_to_file_time));
    register_function("KERNEL32.DLL", "FileTimeToSystemTime", reinterpret_cast<void*>(&hle_file_time_to_system_time));

    // Registry (additional)
    register_function("ADVAPI32.dll", "RegOpenKeyExA",    reinterpret_cast<void*>(&hle_reg_open_key_ex_a));
    register_function("ADVAPI32.dll", "RegQueryValueExA", reinterpret_cast<void*>(&hle_reg_query_value_ex_a));
    register_function("ADVAPI32.dll", "RegCloseKey",      reinterpret_cast<void*>(&hle_reg_close_key));
    register_function("ADVAPI32.dll", "RegSetValueExA",   reinterpret_cast<void*>(&hle_reg_set_value_ex_a));

    // File system additions
    register_function("KERNEL32.DLL", "CreateDirectoryA",  reinterpret_cast<void*>(&hle_create_directory_a));
    register_function("KERNEL32.DLL", "RemoveDirectoryA",  reinterpret_cast<void*>(&hle_remove_directory_a));
    register_function("KERNEL32.DLL", "DeleteFileA",       reinterpret_cast<void*>(&hle_delete_file_a));
    register_function("KERNEL32.DLL", "CopyFileA",         reinterpret_cast<void*>(&hle_copy_file_a));
    register_function("KERNEL32.DLL", "MoveFileA",         reinterpret_cast<void*>(&hle_move_file_a));
    register_function("KERNEL32.DLL", "GetTempPathA",      reinterpret_cast<void*>(&hle_get_temp_path_a));
    register_function("KERNEL32.DLL", "GetTempFileNameA",  reinterpret_cast<void*>(&hle_get_temp_file_name_a));
    register_function("KERNEL32.DLL", "GetWindowsDirectoryA", reinterpret_cast<void*>(&hle_get_windows_directory_a));
    register_function("KERNEL32.DLL", "GetSystemDirectoryA",  reinterpret_cast<void*>(&hle_get_system_directory_a));
    register_function("KERNEL32.DLL", "GetComputerNameA",  reinterpret_cast<void*>(&hle_get_computer_name_a));
    register_function("KERNEL32.DLL", "GetEnvironmentStrings",    reinterpret_cast<void*>(&hle_get_environment_strings));
    register_function("KERNEL32.DLL", "FreeEnvironmentStringsA",  reinterpret_cast<void*>(&hle_free_environment_strings_a));
    register_function("KERNEL32.DLL", "SetErrorMode",     reinterpret_cast<void*>(&hle_set_error_mode));
    register_function("KERNEL32.DLL", "RaiseException",   reinterpret_cast<void*>(&hle_raise_exception));

    // ADVAPI32 - user identity
    register_function("ADVAPI32.dll", "GetUserNameA",     reinterpret_cast<void*>(&hle_get_user_name_a));

    // Module additions
    register_function("KERNEL32.DLL", "GetModuleFileNameW", reinterpret_cast<void*>(&hle_get_module_file_name_w));
    register_function("KERNEL32.DLL", "LoadLibraryExA",   reinterpret_cast<void*>(&hle_load_library_ex_a));
    register_function("KERNEL32.DLL", "LoadLibraryExW",   reinterpret_cast<void*>(&hle_load_library_ex_w));
    register_function("KERNEL32.DLL", "GetVersion",       reinterpret_cast<void*>(&hle_get_version));
    register_function("KERNEL32.DLL", "GetVersionExA",    reinterpret_cast<void*>(&hle_get_version_ex_a));
    register_function("KERNEL32.DLL", "GetVersionExW",    reinterpret_cast<void*>(&hle_get_version_ex_w));
    register_function("KERNEL32.DLL", "GetSystemTimeAsFileTime", reinterpret_cast<void*>(&hle_get_system_time_as_file_time));
    register_function("KERNEL32.DLL", "EncodePointer",    reinterpret_cast<void*>(&generic_stub_arg0));
    register_function("KERNEL32.DLL", "DecodePointer",    reinterpret_cast<void*>(&generic_stub_arg0));
    register_function("KERNEL32.DLL", "GetCurrentProcessorNumber", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("KERNEL32.DLL", "InitializeSListHead", reinterpret_cast<void*>(&generic_stub_success));
    register_function("KERNEL32.DLL", "InterlockedFlushSList", reinterpret_cast<void*>(&generic_stub_null));

    // WINMM.DLL
    register_function("WINMM.DLL", "timeGetTime",         reinterpret_cast<void*>(&hle_time_get_time));
    register_function("WINMM.DLL", "timeBeginPeriod",     reinterpret_cast<void*>(&hle_time_begin_period));
    register_function("WINMM.DLL", "timeEndPeriod",       reinterpret_cast<void*>(&hle_time_end_period));

    // SHELL32.DLL
    register_function("SHELL32.dll", "SHGetFolderPathA",  reinterpret_cast<void*>(&hle_sh_get_folder_path_a));
    register_function("SHELL32.dll", "SHGetFolderPathW",  reinterpret_cast<void*>(&hle_sh_get_folder_path_w));
    register_function("SHELL32.dll", "SHGetKnownFolderPath", reinterpret_cast<void*>(&hle_sh_get_known_folder_path));
    register_function("SHELL32.dll", "CommandLineToArgvW", reinterpret_cast<void*>(&hle_command_line_to_argv_w));

    // OLE32.DLL & OLEAUT32.DLL
    register_function("OLE32.dll", "CoInitialize",        reinterpret_cast<void*>(&hle_co_initialize));
    register_function("OLE32.dll", "CoInitializeEx",      reinterpret_cast<void*>(&hle_co_initialize_ex));
    register_function("OLE32.dll", "CoUninitialize",      reinterpret_cast<void*>(&hle_co_uninitialize));
    register_function("OLE32.dll", "CoCreateInstance",    reinterpret_cast<void*>(&hle_co_create_instance));
    register_function("OLE32.dll", "CoTaskMemAlloc",      reinterpret_cast<void*>(&hle_co_task_mem_alloc));
    register_function("OLE32.dll", "CoTaskMemFree",       reinterpret_cast<void*>(&hle_co_task_mem_free));

    // GDI32.DLL
    register_function("GDI32.DLL", "ChoosePixelFormat",   reinterpret_cast<void*>(&hle_choose_pixel_format));
    register_function("GDI32.DLL", "SetPixelFormat",      reinterpret_cast<void*>(&hle_set_pixel_format));
    register_function("GDI32.DLL", "DescribePixelFormat", reinterpret_cast<void*>(&hle_describe_pixel_format));
    register_function("GDI32.DLL", "SwapBuffers",         reinterpret_cast<void*>(&hle_swap_buffers));

    // OPENGL32.DLL & VULKAN-1.DLL
    register_function("OPENGL32.dll", "wglGetProcAddress", reinterpret_cast<void*>(&hle_wgl_get_proc_address));
    register_function("vulkan-1.dll", "vkGetInstanceProcAddr", reinterpret_cast<void*>(&hle_vk_get_instance_proc_addr));

    // WS2_32.DLL
    register_function("WS2_32.dll", "WSAStartup",         reinterpret_cast<void*>(&hle_wsa_startup));
    register_function("WS2_32.dll", "WSACleanup",         reinterpret_cast<void*>(&hle_wsa_cleanup));
    register_function("WS2_32.dll", "WSAGetLastError",    reinterpret_cast<void*>(&hle_wsa_get_last_error));
    register_function("WS2_32.dll", "socket",             reinterpret_cast<void*>(&hle_socket));
    register_function("WS2_32.dll", "closesocket",        reinterpret_cast<void*>(&hle_closesocket));
    register_function("WS2_32.dll", "connect",            reinterpret_cast<void*>(&hle_connect));
    register_function("WS2_32.dll", "send",               reinterpret_cast<void*>(&hle_send));
    register_function("WS2_32.dll", "recv",               reinterpret_cast<void*>(&hle_recv));
    register_function("WS2_32.dll", "htons",              reinterpret_cast<void*>(&hle_htons));
    register_function("WS2_32.dll", "htonl",              reinterpret_cast<void*>(&hle_htonl));
    register_function("WS2_32.dll", "ntohs",              reinterpret_cast<void*>(&hle_ntohs));
    register_function("WS2_32.dll", "ntohl",              reinterpret_cast<void*>(&hle_ntohl));

    // USER32.DLL additions
    register_function("USER32.DLL", "GetCursorPos",       reinterpret_cast<void*>(&hle_get_cursor_pos));
    register_function("USER32.DLL", "SetCursorPos",       reinterpret_cast<void*>(&hle_set_cursor_pos));
    register_function("USER32.DLL", "ShowCursor",         reinterpret_cast<void*>(&hle_show_cursor));
    register_function("USER32.DLL", "GetAsyncKeyState",    reinterpret_cast<void*>(&hle_get_async_key_state));
    register_function("USER32.DLL", "GetKeyState",        reinterpret_cast<void*>(&hle_get_key_state));
    register_function("USER32.DLL", "GetKeyboardState",   reinterpret_cast<void*>(&hle_get_keyboard_state));
    register_function("USER32.DLL", "SetWindowTextA",     reinterpret_cast<void*>(&hle_set_window_text_a));
    register_function("USER32.DLL", "SetWindowTextW",     reinterpret_cast<void*>(&hle_set_window_text_w));
    register_function("USER32.DLL", "GetWindowTextA",     reinterpret_cast<void*>(&hle_get_window_text_a));
    register_function("USER32.DLL", "GetWindowTextW",     reinterpret_cast<void*>(&hle_get_window_text_w));
    register_function("USER32.DLL", "AdjustWindowRect",   reinterpret_cast<void*>(&hle_adjust_window_rect));
    register_function("USER32.DLL", "AdjustWindowRectEx", reinterpret_cast<void*>(&hle_adjust_window_rect_ex));
    register_function("USER32.DLL", "SetWindowPos",       reinterpret_cast<void*>(&hle_set_window_pos));
    register_function("USER32.DLL", "CreateWindowExW",    reinterpret_cast<void*>(&hle_create_window_ex_a));
    register_function("USER32.DLL", "DefWindowProcW",     reinterpret_cast<void*>(&hle_def_window_proc_a));
    register_function("USER32.DLL", "RegisterClassW",     reinterpret_cast<void*>(&hle_register_class_a));
    register_function("USER32.DLL", "RegisterClassExW",   reinterpret_cast<void*>(&hle_register_class_a));
    register_function("USER32.DLL", "SetCursor",          reinterpret_cast<void*>(&generic_stub_null));
    register_function("USER32.DLL", "GetForegroundWindow",reinterpret_cast<void*>(&generic_stub_null));
    register_function("USER32.DLL", "SetForegroundWindow",reinterpret_cast<void*>(&generic_stub_success));
    register_function("USER32.DLL", "GetActiveWindow",    reinterpret_cast<void*>(&generic_stub_null));
    register_function("USER32.DLL", "SetActiveWindow",    reinterpret_cast<void*>(&generic_stub_null));
    register_function("USER32.DLL", "GetFocus",           reinterpret_cast<void*>(&generic_stub_null));
    register_function("USER32.DLL", "SetFocus",           reinterpret_cast<void*>(&generic_stub_null));
    register_function("USER32.DLL", "GetCapture",         reinterpret_cast<void*>(&generic_stub_null));
    register_function("USER32.DLL", "SetCapture",         reinterpret_cast<void*>(&generic_stub_null));
    register_function("USER32.DLL", "ReleaseCapture",     reinterpret_cast<void*>(&generic_stub_success));

    // DXGI.DLL & D3D11.DLL & DINPUT8.DLL
    register_function("DXGI.DLL", "CreateDXGIFactory",    reinterpret_cast<void*>(&generic_stub_zero));
    register_function("DXGI.DLL", "CreateDXGIFactory1",   reinterpret_cast<void*>(&generic_stub_zero));
    register_function("DXGI.DLL", "CreateDXGIFactory2",   reinterpret_cast<void*>(&generic_stub_zero));
    register_function("D3D11.DLL", "D3D11CreateDevice",   reinterpret_cast<void*>(&generic_stub_zero));
    register_function("D3D11.DLL", "D3D11CreateDeviceAndSwapChain", reinterpret_cast<void*>(&generic_stub_zero));
    register_function("DINPUT8.DLL", "DirectInput8Create", reinterpret_cast<void*>(&generic_stub_zero));

    return {};
}

void Win32ApiHle::register_function(std::string_view dll_name, std::string_view function_name, void* func_ptr) {
    std::string upper_dll(dll_name);
    for (auto& c : upper_dll) c = static_cast<char>(std::toupper(c));
    export_table_[upper_dll][std::string(function_name)] = func_ptr;
}

void* Win32ApiHle::resolve_symbol(std::string_view dll_name, std::string_view function_name) {
    std::string upper_dll(dll_name);
    for (auto& c : upper_dll) c = static_cast<char>(std::toupper(c));

    auto dll_it = export_table_.find(upper_dll);
    if (dll_it != export_table_.end()) {
        auto func_it = dll_it->second.find(std::string(function_name));
        if (func_it != dll_it->second.end()) {
            return func_it->second;
        }
    }

    // Default safe fallback stub instead of returning nullptr
    return reinterpret_cast<void*>(&generic_stub_zero);
}

} // namespace papaya::win32
