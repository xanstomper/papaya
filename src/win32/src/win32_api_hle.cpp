#include "papaya/win32/win32_api_hle.hpp"
#include "papaya/common/logger.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <csignal>
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

namespace papaya::win32 {

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

// Generic no-op stub for uncritical APIs
static PAPAYA_MS_ABI void* generic_stub_success() { return reinterpret_cast<void*>(1); }
static PAPAYA_MS_ABI void* generic_stub_null() { return nullptr; }
static PAPAYA_MS_ABI int   generic_stub_zero() { return 0; }
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

struct ThreadParamBridge {
    void* lpStart;
    void* lpParam;
};

static void* thread_trampoline(void* arg) {
    auto* tp = static_cast<ThreadParamBridge*>(arg);
    auto fn = reinterpret_cast<u32 (*)(void*)>(tp->lpStart);
    void* param = tp->lpParam;
    delete tp;
    u32 ret = fn(param);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(ret));
}

HANDLE Win32ApiHle::hle_create_thread(void* lpSec, size_t dwStack, void* lpStart, void* lpParam, u32 dwFlags, u32* lpId) {
    pthread_t thread;
    auto* tp = new ThreadParamBridge{lpStart, lpParam};
    if (pthread_create(&thread, nullptr, thread_trampoline, tp) == 0) {
        if (lpId) *lpId = static_cast<u32>(thread);
        return reinterpret_cast<HANDLE>(thread);
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
    auto* ev = static_cast<NativeEventState*>(hHandle);
    std::unique_lock<std::mutex> lock(ev->mtx);

    if (ev->signaled) {
        if (!ev->manual_reset) ev->signaled = false;
        return 0;
    }

    if (dwMilliseconds == 0) return 0x102;

    if (dwMilliseconds == 0xFFFFFFFF) {
        ev->cv.wait(lock, [&]() { return ev->signaled; });
        if (!ev->manual_reset) ev->signaled = false;
        return 0;
    }

    bool res = ev->cv.wait_for(lock, std::chrono::milliseconds(dwMilliseconds), [&]() { return ev->signaled; });
    if (res) {
        if (!ev->manual_reset) ev->signaled = false;
        return 0;
    }
    return 0x102;
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
    return nullptr;
}

void* Win32ApiHle::hle_get_module_handle_a(const char* lpModuleName) {
    return reinterpret_cast<void*>(0x140000000);
}

void* Win32ApiHle::hle_get_module_handle_w(const wchar_t* lpModuleName) {
    return reinterpret_cast<void*>(0x140000000);
}

void* Win32ApiHle::hle_load_library_a(const char* lpLibFileName) {
    return reinterpret_cast<void*>(0x180000000);
}

void* Win32ApiHle::hle_load_library_w(const wchar_t* lpLibFileName) {
    return reinterpret_cast<void*>(0x180000000);
}

BOOL Win32ApiHle::hle_free_library(void* hLibModule) {
    return TRUE_VAL;
}

u32 Win32ApiHle::hle_get_module_file_name_a(void* hModule, char* lpFilename, u32 nSize) {
    if (lpFilename && nSize > 10) {
        std::strncpy(lpFilename, "C:\\game.exe", nSize);
        return 11;
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
    if (nIndex == 0) return 1920;
    if (nIndex == 1) return 1080;
    return 0;
}

BOOL Win32ApiHle::hle_set_process_dpi_aware() { return TRUE_VAL; }
BOOL Win32ApiHle::hle_get_client_rect(HWND hWnd, void* lpRect) { return TRUE_VAL; }
BOOL Win32ApiHle::hle_get_window_rect(HWND hWnd, void* lpRect) { return TRUE_VAL; }
BOOL Win32ApiHle::hle_peek_message_a(void* lpMsg, HWND hWnd, u32 wMsgFilterMin, u32 wMsgFilterMax, u32 wRemoveMsg) { return FALSE_VAL; }
BOOL Win32ApiHle::hle_dispatch_message_a(const void* lpMsg) { return TRUE_VAL; }
BOOL Win32ApiHle::hle_translate_message(const void* lpMsg) { return TRUE_VAL; }

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
int  Win32ApiHle::hle_msvcrt__onexit(void*)          { return 0; }
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

// __iob_func(): returns the FILE* array for the std streams. Old mingw uses
// this to reach stdin/stdout/stderr. We return six system-valid FILE slots
// backed by fd 0/1/2 so stdio works.
void* Win32ApiHle::hle_msvcrt___iob_func() {
    static FILE* iob[3] = { stdout, stderr, stdin }; // some runtimes expect stdin at 0
    // Reorder to [stdin, stdout, stderr]-order semantics with 20-byte stride for
    // the MSVCRT _iob[] legacy layout; safest is to alias to host stdout/err.
    FILE** arr = static_cast<FILE**>(iob);
    arr[0] = stdin; arr[1] = stdout; arr[2] = stderr;
    return arr;
}

// ---- stdio ----
int Win32ApiHle::hle_msvcrt_printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap); return r;
}
int Win32ApiHle::hle_msvcrt_fprintf(void* stream, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    FILE* f = (stream == reinterpret_cast<void*>(1)) ? stdout :
              (stream == reinterpret_cast<void*>(2)) ? stderr : static_cast<FILE*>(stream);
    int r = vfprintf(f ? f : stdout, fmt, ap);
    va_end(ap); return r;
}
int Win32ApiHle::hle_msvcrt_vfprintf(void* stream, const char* fmt, va_list ap) {
    FILE* f = (stream == reinterpret_cast<void*>(1)) ? stdout :
              (stream == reinterpret_cast<void*>(2)) ? stderr : static_cast<FILE*>(stream);
    // On x64 the guest passes its va_list by pointer value into `ap`.
    return vfprintf(f ? f : stdout, fmt, ap);
}
int Win32ApiHle::hle_msvcrt_sprintf(char* buf, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap); return r;
}
size_t Win32ApiHle::hle_msvcrt_fwrite(const void* ptr, size_t sz, size_t n, void* stream) {
    FILE* f = (stream == reinterpret_cast<void*>(1)) ? stdout :
              (stream == reinterpret_cast<void*>(2)) ? stderr : static_cast<FILE*>(stream);
    return fwrite(ptr, sz, n, f ? f : stdout);
}
int Win32ApiHle::hle_msvcrt_puts(const char* s)    { return puts(s); }
int Win32ApiHle::hle_msvcrt_fputs(const char* s, void* stream) {
    FILE* f = (stream == reinterpret_cast<void*>(1)) ? stdout :
              (stream == reinterpret_cast<void*>(2)) ? stderr : static_cast<FILE*>(stream);
    return fputs(s, f ? f : stdout);
}
int Win32ApiHle::hle_msvcrt_fputc(int c, void* stream) {
    FILE* f = (stream == reinterpret_cast<void*>(1)) ? stdout :
              (stream == reinterpret_cast<void*>(2)) ? stderr : static_cast<FILE*>(stream);
    return fputc(c, f ? f : stdout);
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
// Win32ApiHle Initialization & Registration Matrix
// -------------------------------------------------------------
Win32ApiHle::Win32ApiHle(
    std::shared_ptr<steam::SteamApiStub> steam_stub,
    std::shared_ptr<input::VirtualXInputManager> input_mgr
) : steam_stub_(steam_stub),
    input_mgr_(input_mgr) {
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

    // USER32.DLL & GDI32.DLL
    register_function("USER32.DLL", "GetSystemMetrics", reinterpret_cast<void*>(&hle_get_system_metrics));
    register_function("USER32.DLL", "SetProcessDPIAware", reinterpret_cast<void*>(&hle_set_process_dpi_aware));
    register_function("USER32.DLL", "GetClientRect", reinterpret_cast<void*>(&hle_get_client_rect));
    register_function("USER32.DLL", "GetWindowRect", reinterpret_cast<void*>(&hle_get_window_rect));
    register_function("USER32.DLL", "PeekMessageA", reinterpret_cast<void*>(&hle_peek_message_a));
    register_function("USER32.DLL", "DispatchMessageA", reinterpret_cast<void*>(&hle_dispatch_message_a));
    register_function("USER32.DLL", "TranslateMessage", reinterpret_cast<void*>(&hle_translate_message));

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
