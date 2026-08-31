#include <shared_mutex>
#include "papaya/win32/win32_api_hle.hpp"
#include "papaya/win32/win32_d3d.hpp"
#include "papaya/win32/win32_dsound.hpp"
#include "papaya/win32/win32_dinput.hpp"
#include "papaya/win32/win32_mmdevice.hpp"
#include "papaya/win32/win32_gl.hpp"
#include "papaya/win32/win32_registry.hpp"
#include "papaya/win32/win32_audio.hpp"
#include "papaya/win32/win32_window.hpp"
#include "papaya/win32/win32_seh.hpp"
#include "papaya/win32/pe_loader.hpp"
#include "papaya/common/logger.hpp"
#include <filesystem>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <fcntl.h>
#include <time.h>
#include <cstring>
#include <cstdlib>
#include <fstream>
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
#include <unordered_set>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <langinfo.h>
#include <clocale>
#include <array>
#include <X11/Xlib.h>
#include <errno.h>
#include <dlfcn.h>
#include <dirent.h>
#include <fnmatch.h>

namespace papaya::win32 {
// Proper UTF-16 (Windows WCHAR = 2 bytes) string conversion helpers:
static inline size_t win_copy_u16(void* dst, const char16_t* src, size_t max_chars) {
    if (!dst || max_chars == 0) return 0;
    auto* d = static_cast<char16_t*>(dst);
    size_t i = 0;
    while (src[i] && i + 1 < max_chars) {
        d[i] = src[i];
        i++;
    }
    d[i] = 0;
    return i;
}

static size_t win_utf16_len(const void* ptr) {
    if (!ptr) return 0;
    const uint16_t* u16 = static_cast<const uint16_t*>(ptr);
    size_t len = 0;
    while (*u16++) len++;
    return len;
}

static std::string win_utf16_to_utf8(const void* ptr, int cch_wc = -1) {
    if (!ptr) return "";
    uintptr_t val = reinterpret_cast<uintptr_t>(ptr);
    if (val < 0x10000) {
        return "#" + std::to_string(val);
    }
    const uint16_t* u16 = static_cast<const uint16_t*>(ptr);
    std::string out;
    size_t count = 0;
    while (*u16) {
        if (cch_wc >= 0 && static_cast<int>(count) >= cch_wc) break;
        uint16_t ch = *u16++;
        count++;
        if (ch < 0x80) {
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    return out;
}

static std::string wchar_to_utf8(const wchar_t* w) {
    return win_utf16_to_utf8(w);
}

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

// Map a guest FILE* back to the host FILE*.
// Two honest sources exist:
//   1. The emulated _iob slot array (stdin/stdout/stderr via __iob_func),
//      where each 48-byte slot carries the host FILE* in its first 8 bytes.
//   2. Streams the msvcrt fopen family handed to the guest: those are host
//      FILE* values recorded in g_open_files at open time.
// Anything else (arbitrary guest pointers) resolves to nullptr so callers
// fail gracefully instead of dereferencing guest memory as a FILE*.
static std::mutex g_files_mutex;
static std::unordered_set<FILE*>& open_host_files() {
    static std::unordered_set<FILE*> set;
    return set;
}
static void register_host_file(FILE* f) {
    if (!f) return;
    std::lock_guard<std::mutex> lk(g_files_mutex);
    open_host_files().insert(f);
}
static void unregister_host_file(FILE* f) {
    if (!f) return;
    std::lock_guard<std::mutex> lk(g_files_mutex);
    open_host_files().erase(f);
}
static FILE* host_file_for(void* guest_file) {
    if (!guest_file) return nullptr;
    unsigned char* slot = static_cast<unsigned char*>(guest_file);
    if (slot >= g_iob_slots && slot < g_iob_slots + sizeof(g_iob_slots)) {
        size_t off = static_cast<size_t>(slot - g_iob_slots);
        slot = g_iob_slots + (off / kGuestFileStride) * kGuestFileStride;
        FILE* hf = nullptr;
        std::memcpy(&hf, slot, sizeof(hf));
        return hf;
    }
    std::lock_guard<std::mutex> lk(g_files_mutex);
    FILE* key = static_cast<FILE*>(guest_file);
    return open_host_files().count(key) ? key : nullptr;
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
// Codegen attribution-stub thunks (scripts/gen_thunks.py). Return-safety matched
// benign defaults so optional / feature-absent APIs resolve without crashing.
static PAPAYA_MS_ABI int   codegen_stub_zero() { return 0; }
static PAPAYA_MS_ABI void* codegen_stub_null() { return nullptr; }
static PAPAYA_MS_ABI int   codegen_stub_true() { return 1; }
static PAPAYA_MS_ABI void* codegen_stub_arg0(void* p) { return p; }
static PAPAYA_MS_ABI long  codegen_stub_hresult_ok() { return 0; }        /* S_OK */
static PAPAYA_MS_ABI long  codegen_stub_hresult_fail() { return 0x80004001L; } /* E_NOTIMPL */
static PAPAYA_MS_ABI u32 hle_get_dpi_for_window(void* hWnd) { (void)hWnd; return 96; }
static PAPAYA_MS_ABI u32 hle_get_dpi_for_system() { return 96; }
static PAPAYA_MS_ABI int hle_get_dpi_for_monitor(void* hmonitor, int dpiType, u32* dpiX, u32* dpiY) {
    (void)hmonitor; (void)dpiType;
    if (dpiX) *dpiX = 96;
    if (dpiY) *dpiY = 96;
    return 0; // S_OK
}
static PAPAYA_MS_ABI BOOL hle_get_pointer_type(u32 pointerId, u32* pointerType) {
    (void)pointerId;
    if (pointerType) *pointerType = 0; // PT_POINTER
    return TRUE_VAL;
}
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
static std::mutex g_vmem_mutex;
static std::unordered_map<void*, size_t> g_vmem_allocs;

void* Win32ApiHle::hle_virtual_alloc(void* lpAddress, size_t dwSize, u32 flAllocationType, u32 flProtect) {
    if (dwSize == 0) dwSize = 4096;
    dwSize = (dwSize + 4095) & ~static_cast<size_t>(4095);
    int prot = PROT_READ | PROT_WRITE;
    if (flProtect == 0x40 || flProtect == 0x20) prot |= PROT_EXEC;

    void* ptr = mmap(lpAddress, dwSize, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        g_last_error = 8;
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(g_vmem_mutex);
        g_vmem_allocs[ptr] = dwSize;
    }
    return ptr;
}

BOOL Win32ApiHle::hle_virtual_free(void* lpAddress, size_t dwSize, u32 dwFreeType) {
    if (!lpAddress) return FALSE_VAL;
    size_t unmap_sz = dwSize;
    {
        std::lock_guard<std::mutex> lock(g_vmem_mutex);
        auto it = g_vmem_allocs.find(lpAddress);
        if (it != g_vmem_allocs.end()) {
            if (unmap_sz == 0 || (dwFreeType & 0x8000 /* MEM_RELEASE */)) {
                unmap_sz = it->second;
            }
            g_vmem_allocs.erase(it);
        }
    }
    if (unmap_sz > 0) {
        munmap(lpAddress, (unmap_sz + 4095) & ~static_cast<size_t>(4095));
    }
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_virtual_protect(void* lpAddress, size_t dwSize, u32 flNewProtect, u32* lpflOldProtect) {
    if (lpflOldProtect) *lpflOldProtect = 0x04;
    int prot = PROT_READ | PROT_WRITE;
    if (flNewProtect == 0x40 || flNewProtect == 0x20) prot |= PROT_EXEC;
    return (mprotect(lpAddress, dwSize, prot) == 0) ? TRUE_VAL : FALSE_VAL;
}

// Memory-mapped file support: CreateFileMapping -> MapViewOfFile ->
// UnmapViewOfFile backed by host mmap over the guest fd (CreateFile HANDLE).
// The fd handle from hle_create_file_a is stored directly; mappings are kept
// so UnmapViewOfFile can munmap by base address. Note: guest "handle" is the
// host fd int; hFile==(void*)-1 means anonymous (page-file backed) mapping.
static std::mutex g_mmap_mutex;
static std::unordered_map<void*, size_t> g_mmap_views;
static std::unordered_map<HANDLE, size_t> g_mapping_sizes;  // hMapping -> bytes

HANDLE Win32ApiHle::hle_create_file_mapping_a(void* hFile, void* lpFileMappingAttributes, u32 flProtect, u32 dwMaximumSizeHigh, u32 dwMaximumSizeLow, const char* lpName) {
    (void)lpFileMappingAttributes; (void)flProtect; (void)lpName;
    u64 size = (u64(dwMaximumSizeHigh) << 32) | dwMaximumSizeLow;
    if (size == 0) {
        // Size 0 => map the whole file: query its length.
        if (hFile && hFile != reinterpret_cast<void*>(-1)) {
            struct stat st{}; if (fstat(static_cast<int>(reinterpret_cast<uintptr_t>(hFile)), &st) == 0) size = static_cast<u64>(st.st_size);
        }
        if (size == 0) size = 4096;
    }
    // Return a tokenized handle: use the (fd if valid), else a synthetic id.
    HANDLE h;
    if (hFile && hFile != reinterpret_cast<void*>(-1)) h = hFile;
    else h = reinterpret_cast<HANDLE>(0x2000 + (g_mapping_sizes.size() % 64));
    {
        std::lock_guard<std::mutex> lock(g_mmap_mutex);
        g_mapping_sizes[h] = static_cast<size_t>(size);
    }
    return h;
}

HANDLE Win32ApiHle::hle_create_file_mapping_w(void* hFile, void* lpFileMappingAttributes, u32 flProtect, u32 dwMaximumSizeHigh, u32 dwMaximumSizeLow, const wchar_t* lpName) {
    (void)lpName;
    return hle_create_file_mapping_a(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, nullptr);
}

void* Win32ApiHle::hle_map_view_of_file(void* hMappingObject, u32 dwDesiredAccess, u32 dwFileOffsetHigh, u32 dwFileOffsetLow, size_t dwNumberOfBytesToMap) {
    (void)dwDesiredAccess;
    u64 offset = (u64(dwFileOffsetHigh) << 32) | dwFileOffsetLow;
    size_t bytes = dwNumberOfBytesToMap;
    size_t mapping_size = 0;
    HANDLE fd_handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mmap_mutex);
        auto it = g_mapping_sizes.find(hMappingObject);
        if (it != g_mapping_sizes.end()) mapping_size = it->second;
        fd_handle = hMappingObject;
    }
    int fd = -1;
    bool anonymous = (reinterpret_cast<uintptr_t>(fd_handle) >= 0x2000);
    if (!anonymous) fd = static_cast<int>(reinterpret_cast<uintptr_t>(fd_handle));
    if (bytes == 0) bytes = (mapping_size > offset) ? mapping_size - static_cast<size_t>(offset) : mapping_size;

    void* addr = nullptr;
    if (anonymous) {
        addr = mmap(nullptr, (bytes + 4095) & ~4095ul, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    } else {
        void* base = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, static_cast<off_t>(offset));
        addr = base;
    }
    if (addr == MAP_FAILED) { g_last_error = 8; return nullptr; }
    {
        std::lock_guard<std::mutex> lock(g_mmap_mutex);
        g_mmap_views[addr] = bytes;
    }
    return addr;
}

void* Win32ApiHle::hle_map_view_of_file_ex(HANDLE hMappingObject, u32 dwDesiredAccess, u64 dwFileOffset, size_t dwNumberOfBytesToMap, void* lpBaseAddress, u32 dwFlags) {
    (void)lpBaseAddress; (void)dwFlags;
    return hle_map_view_of_file(hMappingObject, dwDesiredAccess,
                                static_cast<u32>(dwFileOffset >> 32), static_cast<u32>(dwFileOffset & 0xFFFFFFFF),
                                dwNumberOfBytesToMap);
}

BOOL Win32ApiHle::hle_unmap_view_of_file(void* lpBaseAddress) {
    if (!lpBaseAddress) return FALSE_VAL;
    size_t bytes = 0;
    {
        std::lock_guard<std::mutex> lock(g_mmap_mutex);
        auto it = g_mmap_views.find(lpBaseAddress);
        if (it != g_mmap_views.end()) { bytes = it->second; g_mmap_views.erase(it); }
    }
    if (bytes > 0) munmap(lpBaseAddress, bytes);
    return TRUE_VAL;
}

int Win32ApiHle::hle_lstrcmp_w(const wchar_t* a, const wchar_t* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    const uint16_t* A = reinterpret_cast<const uint16_t*>(a);
    const uint16_t* B = reinterpret_cast<const uint16_t*>(b);
    for (;;) { if (*A != *B) return (int)*A - (int)*B; if (*A == 0) return 0; ++A; ++B; }
}
int Win32ApiHle::hle_lstrcmpi_a(const char* a, const char* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return strcasecmp(a, b);
}
int Win32ApiHle::hle_lstrcmpi_w(const wchar_t* a, const wchar_t* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    const uint16_t* A = reinterpret_cast<const uint16_t*>(a);
    const uint16_t* B = reinterpret_cast<const uint16_t*>(b);
    for (;;) { uint16_t ca = (uint16_t)((*A >= 'A' && *A <= 'Z') ? *A + 32 : *A);
               uint16_t cb = (uint16_t)((*B >= 'A' && *B <= 'Z') ? *B + 32 : *B);
               if (ca != cb) return (int)ca - (int)cb; if (ca == 0) return 0; ++A; ++B; }
}
int Win32ApiHle::hle_mul_div(int nNumber, int nNumerator, int nDenominator) {
    if (nDenominator == 0) { g_last_error = 11 /*DIVIDE_BY_ZERO*/; return -1; }
    return static_cast<int>((static_cast<long long>(nNumber) * nNumerator) / nDenominator);
}
BOOL Win32ApiHle::hle_is_wow64_process(void* hProcess, void* lpfIsWow64Process) {
    (void)hProcess;
    if (lpfIsWow64Process) *static_cast<BOOL*>(lpfIsWow64Process) = FALSE_VAL;
    return TRUE_VAL;   // x64-native, not running under WoW64
}
BOOL Win32ApiHle::hle_is_bad_string_ptr_a(const void* ptr, u32 size) {
    if (!ptr) return TRUE_VAL;
    if (size != 0) { return (memchr(ptr, 0, size) == nullptr) ? TRUE_VAL : FALSE_VAL; }
    const char* p = static_cast<const char*>(ptr); const char* e = p + 4096;
    while (p < e) { if (*p == 0) return FALSE_VAL; ++p; }
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_is_bad_string_ptr_w(const void* ptr, u32 size) {
    if (!ptr) return TRUE_VAL;
    const uint16_t* p = static_cast<const uint16_t*>(ptr);
    const uint16_t* e = p + (size ? size / 2 : 2048);
    while (p < e) { if (*p == 0) return FALSE_VAL; ++p; }
    return TRUE_VAL;
}
u32 Win32ApiHle::hle_get_temp_path_w(u32 nBufferLength, wchar_t* lpBuffer) {
    const char* tmp = getenv("TMPDIR"); if (!tmp || !*tmp) tmp = "/tmp";
    uint16_t* dst = reinterpret_cast<uint16_t*>(lpBuffer);
    size_t i = 0;
    while (tmp[i] && i + 1 < nBufferLength) { dst[i] = (uint16_t)(unsigned char)tmp[i]; ++i; }
    if (nBufferLength > i) dst[i++] = 0;
    return static_cast<u32>(i);
}
u32 Win32ApiHle::hle_get_system_directory_w(void* lpBuffer, u32 nSize) {
    // Expose the papaya_prefix drive_c\windows as the system dir.
    const char* sys = "C:\\windows";
    uint16_t* dst = reinterpret_cast<uint16_t*>(lpBuffer);
    size_t i = 0;
    while (sys[i] && i + 1 < nSize) { dst[i] = (uint16_t)(unsigned char)sys[i]; ++i; }
    if (i + 1 <= nSize) dst[i++] = 0;
    return static_cast<u32>(i);
}

HANDLE Win32ApiHle::hle_get_process_heap() {
    return reinterpret_cast<HANDLE>(0x1000);
}

struct alignas(16) Win32HeapHeader {
    u32 magic;       // 0x50415059 ("PAPY")
    u32 flags;
    size_t size;
    size_t reserved;
};

void* Win32ApiHle::hle_heap_alloc(HANDLE hHeap, u32 dwFlags, size_t dwBytes) {
    (void)hHeap;
    if (dwBytes == 0) dwBytes = 1;
    size_t total_size = sizeof(Win32HeapHeader) + dwBytes;
    void* ptr = (dwFlags & 8 /* HEAP_ZERO_MEMORY */) ? std::calloc(1, total_size) : std::malloc(total_size);
    if (!ptr) {
        g_last_error = 8; // ERROR_NOT_ENOUGH_MEMORY
        return nullptr;
    }
    auto* hdr = static_cast<Win32HeapHeader*>(ptr);
    hdr->magic = 0x50415059;
    hdr->flags = dwFlags;
    hdr->size = dwBytes;
    hdr->reserved = 0;
    return hdr + 1;
}

BOOL Win32ApiHle::hle_heap_free(HANDLE hHeap, u32 dwFlags, void* lpMem) {
    (void)hHeap; (void)dwFlags;
    if (!lpMem) return TRUE_VAL;
    auto* hdr = reinterpret_cast<Win32HeapHeader*>(lpMem) - 1;
    if (hdr->magic == 0x50415059) {
        hdr->magic = 0;
        std::free(hdr);
        return TRUE_VAL;
    }
    return TRUE_VAL;
}

void* Win32ApiHle::hle_heap_realloc(HANDLE hHeap, u32 dwFlags, void* lpMem, size_t dwBytes) {
    (void)hHeap;
    if (!lpMem) return hle_heap_alloc(hHeap, dwFlags, dwBytes);
    if (dwBytes == 0) { hle_heap_free(hHeap, dwFlags, lpMem); return nullptr; }
    auto* hdr = reinterpret_cast<Win32HeapHeader*>(lpMem) - 1;
    if (hdr->magic != 0x50415059) {
        return hle_heap_alloc(hHeap, dwFlags, dwBytes);
    }
    size_t total_size = sizeof(Win32HeapHeader) + dwBytes;
    void* new_ptr = std::realloc(hdr, total_size);
    if (!new_ptr) return nullptr;
    auto* new_hdr = static_cast<Win32HeapHeader*>(new_ptr);
    new_hdr->size = dwBytes;
    return new_hdr + 1;
}

void* Win32ApiHle::hle_local_alloc(u32 uFlags, size_t uBytes) {
    u32 heap_flags = (uFlags & 0x0040 /* LMEM_ZEROINIT */) ? 8 : 0;
    return hle_heap_alloc(reinterpret_cast<HANDLE>(0x1000), heap_flags, uBytes);
}

void* Win32ApiHle::hle_local_free(void* hMem) {
    if (hMem) hle_heap_free(reinterpret_cast<HANDLE>(0x1000), 0, hMem);
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
    return TRUE_VAL;
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
    while (dwTlsIndex >= g_tls_keys.size()) {
        pthread_key_t key;
        if (pthread_key_create(&key, nullptr) == 0) {
            g_tls_keys.push_back(key);
        } else {
            return FALSE_VAL;
        }
    }
    return (pthread_setspecific(g_tls_keys[dwTlsIndex], lpTlsValue) == 0) ? TRUE_VAL : FALSE_VAL;
}

u32 Win32ApiHle::hle_fls_alloc(void* lpCallback) {
    (void)lpCallback;
    return hle_tls_alloc();
}

BOOL Win32ApiHle::hle_fls_free(u32 dwFlsIndex) {
    return hle_tls_free(dwFlsIndex);
}

void* Win32ApiHle::hle_fls_get_value(u32 dwFlsIndex) {
    return hle_tls_get_value(dwFlsIndex);
}

BOOL Win32ApiHle::hle_fls_set_value(u32 dwFlsIndex, void* lpFlsData) {
    return hle_tls_set_value(dwFlsIndex, lpFlsData);
}

// Handle-type tags so wait/close dispatch correctly (thread vs event vs mutex).
enum : u32 {
    kHandleNone   = 0,
    kHandleThread = 0x54524844,  // "THRD"
    kHandleEvent  = 0x45564E54,  // "EVNT"
    kHandleMutex  = 0x4D545854,  // "MTXT"
    kHandleSem    = 0x53454D41,  // "SEMA"
    kHandleFind   = 0x46494E44,  // "FIND"
};
static u32 handle_tag(void* h) {
    if (!h) return kHandleNone;
    uintptr_t addr = reinterpret_cast<uintptr_t>(h);
    if (addr < 0x10000 || addr >= 0x00007FFFFFFFFFFFULL) return kHandleNone;
    u32 tag = *reinterpret_cast<u32*>(h);
    // Validate against the exact set of known tags (values are not monotonic).
    switch (tag) {
        case kHandleThread: case kHandleEvent: case kHandleMutex: case kHandleSem: case kHandleFind:
            return tag;
        default:
            return kHandleNone;
    }
}

struct NativeFindState {
    u32 tag{kHandleFind};
    DIR* dir{nullptr};
    std::string base_dir;
    std::string pattern;
};

struct NativeThreadState {
    u32       tag{kHandleThread};
    pthread_t handle{};
    pid_t     tid{0};
    std::atomic<bool> finished{false};
    std::mutex mtx;
    std::condition_variable cv;
};

struct ThreadParamBridge {
    void* lpStart;
    void* lpParam;
    NativeThreadState* nts;
    std::atomic<bool> ready{false};
};

// GDI32 software-surface handle types (a DC/bitmap is one of these; the window
// manager owns the per-window backbuffer, compatible DCs/bitmaps allocate their own).
struct GdiDc {
    u32 tag{0x47444943};   // "GDIC"
    int  w{0}, h{0};
    u8*  fb{nullptr};      // borrowed window fb (may be realloc'd) or owned
    u32  fb_size{0};
    bool window_backed{false};
    void* hwnd{nullptr};   // owning native window (re-resolve fb via surface_buffer)
    u32  text_color{0xFF000000};   // RGBA, for TextOutA
    u32  bg_color{0xFFFFFFFF};
    u32  pen_color{0xFF000000};    // COLORREF BGR for LineTo/Rectangle outline
    u32  brush_color{0xFFFFFFFF};  // COLORREF BGR for FillRect/Rectangle fill
    int  pos_x{0}, pos_y{0};       // current pen position
    int  bk_mode{2};               // TRANSPARENT=1, OPAQUE=2
    u32  text_align{0};            // TA_LEFT|TA_TOP
};
struct GdiBitmap {
    u32 tag{0x4744424D};   // "GDBM"
    int  w{0}, h{0};
    u8*  fb{nullptr};
    u32  fb_size{0};
};
static GdiDc* gdi_dc_of(void* h) {
    if (!h) return nullptr;
    uintptr_t addr = reinterpret_cast<uintptr_t>(h);
    if (addr < 0x10000 || addr >= 0x00007FFFFFFFFFFFULL) return nullptr;
    auto* d = static_cast<GdiDc*>(h);
    return (d->tag == 0x47444943) ? d : nullptr;
}
// Current framebuffer for a DC. Window-backed DCs don't cache the pointer
// (surface_buffer can realloc it on resize), so re-resolve through the owner
// window handle to avoid using a freed/dangling buffer.
static u8* gdi_dc_fb(GdiDc* d) {
    if (!d) return nullptr;
    if (d->window_backed && d->hwnd)
        return window_manager().surface_buffer(d->hwnd, d->w, d->h);
    return d->fb;
}
static GdiBitmap* gdi_bmp_of(void* h) {
    if (!h) return nullptr;
    uintptr_t addr = reinterpret_cast<uintptr_t>(h);
    if (addr < 0x10000 || addr >= 0x00007FFFFFFFFFFFULL) return nullptr;
    auto* b = static_cast<GdiBitmap*>(h);
    return (b->tag == 0x4744424D) ? b : nullptr;
}

static void* thread_trampoline(void* arg) {
    auto* tp = static_cast<ThreadParamBridge*>(arg);
    auto fn = reinterpret_cast<u32 (__attribute__((ms_abi))*)(void*)>(tp->lpStart);
    void* param = tp->lpParam;
    auto* nts = tp->nts;
    nts->tid = gettid();
    tp->ready = true;

    // Give the new guest thread its own per-thread TEB + TLS block so
    // Win32 TLS API and __declspec(thread) are thread-local, not shared.
    if (auto* loader = PeLoader::active()) loader->setup_thread_tls();
    seh_install_fault_handler(true);
    u32 ret = 0;
    try {
        ret = fn(param);
    } catch (...) {
        log::warn("WIN32", "Caught unhandled C++ exception on guest worker thread");
    }
    {
        std::lock_guard<std::mutex> lock(nts->mtx);
        nts->finished = true;
    }
    nts->cv.notify_all();
    return reinterpret_cast<void*>(static_cast<uintptr_t>(ret));
}

HANDLE Win32ApiHle::hle_create_thread(void* lpSec, size_t dwStack, void* lpStart, void* lpParam, u32 dwFlags, u32* lpId) {
    auto* nts = new NativeThreadState();
    nts->tag = kHandleThread;

    auto* tp = new ThreadParamBridge{lpStart, lpParam, nts, false};
    pthread_t thread;
    if (pthread_create(&thread, nullptr, thread_trampoline, tp) == 0) {
        nts->handle = thread;
        pthread_detach(thread);
        while (!tp->ready) std::this_thread::yield();
        if (lpId) *lpId = static_cast<u32>(nts->tid);
        delete tp;
        return reinterpret_cast<HANDLE>(nts);
    }
    delete tp;
    delete nts;
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
// CRITICAL_SECTION safety rule: enter/leave/try/delete may only dereference
// mutexes this HLE itself allocated in InitializeCriticalSection*. Guests
// also fire Enter/LeaveCriticalSection at lock objects that were never run
// through our initializer (msvcrt's internal stdio locks, CRT lock tables) —
// their DebugInfo field is guest garbage. Dereferencing it as a recursive_
// mutex* corrupts the host heap (observed: guest stdio locking clobbered
// the malloc chunk header of the FILE buffer -> abort in fclose). Locks we
// never initialized are therefore no-ops: correct for single-threaded
// guests, and a known scope limit for multi-threaded ones.
static std::mutex g_cs_registry_mutex;
static std::unordered_set<void*>& cs_mutex_registry() {
    static std::unordered_set<void*> set;
    return set;
}
static bool cs_mutex_registered(void* mtx) {
    std::lock_guard<std::mutex> lk(g_cs_registry_mutex);
    return cs_mutex_registry().count(mtx) != 0;
}
static void cs_mutex_register(void* mtx) {
    std::lock_guard<std::mutex> lk(g_cs_registry_mutex);
    cs_mutex_registry().insert(mtx);
}
static void cs_mutex_unregister(void* mtx) {
    std::lock_guard<std::mutex> lk(g_cs_registry_mutex);
    cs_mutex_registry().erase(mtx);
}

void Win32ApiHle::hle_init_critical_section(Win32CriticalSection* lpSection) {
    if (!lpSection) return;
    auto* mtx = new std::recursive_mutex();
    lpSection->DebugInfo = mtx;
    cs_mutex_register(mtx);
    lpSection->LockCount = -1;
    lpSection->RecursionCount = 0;
}

BOOL Win32ApiHle::hle_init_critical_section_and_spin_count(Win32CriticalSection* lpSection, u32 dwSpinCount) {
    hle_init_critical_section(lpSection);
    if (lpSection) lpSection->SpinCount = dwSpinCount;
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_init_critical_section_ex(Win32CriticalSection* lpSection, u32 dwSpinCount, u32 flags) {
    (void)dwSpinCount;
    (void)flags;
    hle_init_critical_section(lpSection);
    if (lpSection) lpSection->SpinCount = dwSpinCount;
    return TRUE_VAL;
}

void Win32ApiHle::hle_enter_critical_section(Win32CriticalSection* lpSection) {
    if (!lpSection || !lpSection->DebugInfo) return;
    if (!cs_mutex_registered(lpSection->DebugInfo)) return;  // never-initialized guest lock
    auto* mtx = static_cast<std::recursive_mutex*>(lpSection->DebugInfo);
    mtx->lock();
}

BOOL Win32ApiHle::hle_try_enter_critical_section(Win32CriticalSection* lpSection) {
    if (!lpSection || !lpSection->DebugInfo) return FALSE_VAL;
    if (!cs_mutex_registered(lpSection->DebugInfo)) return TRUE_VAL;  // unowned lock: "acquired"
    auto* mtx = static_cast<std::recursive_mutex*>(lpSection->DebugInfo);
    return mtx->try_lock() ? TRUE_VAL : FALSE_VAL;
}

void Win32ApiHle::hle_leave_critical_section(Win32CriticalSection* lpSection) {
    if (!lpSection || !lpSection->DebugInfo) return;
    if (!cs_mutex_registered(lpSection->DebugInfo)) return;  // never-initialized guest lock
    auto* mtx = static_cast<std::recursive_mutex*>(lpSection->DebugInfo);
    mtx->unlock();
}

void Win32ApiHle::hle_delete_critical_section(Win32CriticalSection* lpSection) {
    if (!lpSection || !lpSection->DebugInfo) return;
    if (!cs_mutex_registered(lpSection->DebugInfo)) return;  // never ours: do not free
    auto* mtx = static_cast<std::recursive_mutex*>(lpSection->DebugInfo);
    cs_mutex_unregister(mtx);
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

struct NativeSemaphoreState {
    u32 tag{kHandleSem};
    std::mutex mtx;
    std::condition_variable cv;
    s32 count{0};
    s32 max_count{1};
};

struct NativeMutexState {
    u32 tag{kHandleMutex};
    std::recursive_mutex mtx;
    bool owned{false};
};

HANDLE Win32ApiHle::hle_create_event_a(void* lpSec, BOOL bManualReset, BOOL bInitialState, const char* lpName) {
    (void)lpSec; (void)lpName;
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
    (void)lpSec; (void)lpName;
    auto* mtx = new NativeMutexState();
    if (bInitialOwner) {
        mtx->mtx.lock();
        mtx->owned = true;
    }
    return reinterpret_cast<HANDLE>(mtx);
}

BOOL Win32ApiHle::hle_release_mutex(HANDLE hMutex) {
    if (!hMutex) return FALSE_VAL;
    if (handle_tag(hMutex) == kHandleMutex) {
        auto* mtx = static_cast<NativeMutexState*>(hMutex);
        mtx->owned = false;
        mtx->mtx.unlock();
        return TRUE_VAL;
    }
    return FALSE_VAL;
}

u32 Win32ApiHle::hle_wait_for_single_object(HANDLE hHandle, u32 dwMilliseconds) {
    if (!hHandle) return 0xFFFFFFFF;
    uintptr_t val = reinterpret_cast<uintptr_t>(hHandle);
    if (val == static_cast<uintptr_t>(-1) || val == static_cast<uintptr_t>(-2)) {
        if (dwMilliseconds > 0 && dwMilliseconds != 0xFFFFFFFF) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(dwMilliseconds, 100u)));
        }
        return 0x102; // WAIT_TIMEOUT (current process/thread is still running)
    }

    switch (handle_tag(hHandle)) {
        case kHandleThread: {
            auto* th = static_cast<NativeThreadState*>(hHandle);
            std::unique_lock<std::mutex> lock(th->mtx);
            if (th->finished.load()) return 0;
            if (dwMilliseconds == 0) return 0x102;
            if (dwMilliseconds == 0xFFFFFFFF) {
                th->cv.wait(lock, [&]() { return th->finished.load(); });
                return 0;
            }
            bool res = th->cv.wait_for(lock, std::chrono::milliseconds(dwMilliseconds),
                                       [&]() { return th->finished.load(); });
            return res ? 0 : 0x102;
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
        case kHandleSem: {
            auto* sem = static_cast<NativeSemaphoreState*>(hHandle);
            std::unique_lock<std::mutex> lock(sem->mtx);
            if (sem->count > 0) {
                sem->count--;
                return 0;
            }
            if (dwMilliseconds == 0) return 0x102;
            if (dwMilliseconds == 0xFFFFFFFF) {
                sem->cv.wait(lock, [&]() { return sem->count > 0; });
                sem->count--;
                return 0;
            }
            bool res = sem->cv.wait_for(lock, std::chrono::milliseconds(dwMilliseconds),
                                       [&]() { return sem->count > 0; });
            if (res) {
                sem->count--;
                return 0;
            }
            return 0x102;
        }
        case kHandleMutex: {
            auto* mtx = static_cast<NativeMutexState*>(hHandle);
            if (dwMilliseconds == 0) {
                if (mtx->mtx.try_lock()) {
                    mtx->owned = true;
                    return 0;
                }
                return 0x102;
            }
            if (dwMilliseconds == 0xFFFFFFFF) {
                mtx->mtx.lock();
                mtx->owned = true;
                return 0;
            }
            auto start = std::chrono::steady_clock::now();
            while (true) {
                if (mtx->mtx.try_lock()) {
                    mtx->owned = true;
                    return 0;
                }
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (static_cast<u32>(elapsed) >= dwMilliseconds) return 0x102;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        default:
            return 0;
    }
}

u32 Win32ApiHle::hle_wait_for_multiple_objects(u32 nCount, const HANDLE* lpHandles, BOOL bWaitAll, u32 dwMilliseconds) {
    if (nCount == 0 || !lpHandles) return 0xFFFFFFFF;
    if (!bWaitAll) {
        for (u32 i = 0; i < nCount; ++i) {
            u32 res = hle_wait_for_single_object(lpHandles[i], 0);
            if (res == 0) return i;
        }
        if (dwMilliseconds == 0) return 0x102;

        auto start = std::chrono::steady_clock::now();
        while (true) {
            for (u32 i = 0; i < nCount; ++i) {
                u32 res = hle_wait_for_single_object(lpHandles[i], 0);
                if (res == 0) return i;
            }
            if (dwMilliseconds != 0xFFFFFFFF) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (static_cast<u32>(elapsed) >= dwMilliseconds) return 0x102;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } else {
        for (u32 i = 0; i < nCount; ++i) {
            u32 res = hle_wait_for_single_object(lpHandles[i], dwMilliseconds);
            if (res != 0) return res;
        }
        return 0;
    }
}

// -------------------------------------------------------------
// File System & Paths
// -------------------------------------------------------------
static std::string normalize_win_path(const char* p) {
    if (!p) return "";
    std::string s(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    // Strip Win32 extended/device path prefixes: \\?\C:\..., \\?\UNC\...,
    // \\.\device\... (Godot 4.3+ generates \\?\ paths for pack lookups).
    bool had_extended = false;
    while (s.rfind("//?/", 0) == 0 || s.rfind("//./", 0) == 0) {
        s = s.substr(4);
        had_extended = true;
    }
    if (s.rfind("UNC/", 0) == 0) s = "//" + s.substr(4);
    if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') {
        std::string stripped = s.substr(2);
        while (!stripped.empty() && stripped[0] == '/') stripped.erase(0, 1);
        if (access(s.c_str(), F_OK) != 0) {
            return stripped.empty() ? "." : stripped;
        }
    }
    // A device-prefixed path with no drive letter ("\\?\home\...", as emitted by
    // Godot when it strips the C: drive from a module path) is a host-absolute
    // path whose leading separator was consumed by the prefix. Re-attach it so
    // these resolve against the real root instead of the process CWD.
    if (had_extended && !s.empty() && s[0] != '/') {
        s = "/" + s;
    }
    return s;
}

// Some Godot 4.3+ titles probe "<name>.exe.pck" while the shipped pack is
// "<name>.pck". Fall back transparently (compat shim; no game files touched).
static bool pck_exe_fallback(const std::string& raw, std::string& out_norm) {
    out_norm = normalize_win_path(raw.c_str());
    struct stat st{};
    if (stat(out_norm.c_str(), &st) == 0) return true;
    std::string lower = out_norm;
    for (auto& c : lower) c = static_cast<char>(std::tolower(c));
    if (lower.size() > 8 && lower.ends_with(".exe.pck")) {
        std::string alt = out_norm.substr(0, out_norm.size() - 8) + ".pck";
        struct stat st2{};
        if (stat(alt.c_str(), &st2) == 0) { out_norm = alt; return true; }
    }
    return false;
}

HANDLE Win32ApiHle::hle_create_file_a(const char* lpFileName, u32 dwAccess, u32 dwShare, void* lpSec, u32 dwDisp, u32 dwFlags, HANDLE hTemplate) {
    (void)dwShare; (void)lpSec; (void)dwFlags; (void)hTemplate;
    std::string path = normalize_win_path(lpFileName);
    int flags = 0;
    if ((dwAccess & 0x40000000) != 0) { // GENERIC_WRITE
        flags = (dwAccess & 0x80000000) ? O_RDWR : O_WRONLY;
    } else {
        flags = O_RDONLY;
    }

    if (dwDisp == 1) flags |= O_CREAT | O_EXCL;       // CREATE_NEW
    else if (dwDisp == 2) flags |= O_CREAT | O_TRUNC;  // CREATE_ALWAYS
    else if (dwDisp == 4) flags |= O_CREAT;            // OPEN_ALWAYS
    else if (dwDisp == 5) flags |= O_TRUNC;            // TRUNCATE_EXISTING

    if (flags & O_CREAT) {
        try {
            std::filesystem::path p(path);
            if (p.has_parent_path()) {
                std::error_code ec;
                std::filesystem::create_directories(p.parent_path(), ec);
            }
        } catch (...) {}
    }

    int fd = open(path.c_str(), flags, 0666);
    if (fd < 0 && (flags & O_RDWR)) {
        fd = open(path.c_str(), O_RDONLY);
    }
    if (fd < 0 && !(flags & O_CREAT)) {
        std::string alt;
        if (pck_exe_fallback(lpFileName ? lpFileName : "", alt) && alt != path) {
            fd = open(alt.c_str(), flags, 0666);
            if (fd >= 0) path = alt;
        }
    }
    log::info("WIN32", "CreateFile: raw='{}' norm='{}' fd={}", lpFileName ? lpFileName : "null", path, fd);
    if (fd < 0) {
        g_last_error = 2; // ERROR_FILE_NOT_FOUND
        return reinterpret_cast<HANDLE>(reinterpret_cast<void*>(-1));
    }
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fd));
}

HANDLE Win32ApiHle::hle_create_file_w(const wchar_t* lpFileName, u32 dwAccess, u32 dwShare, void* lpSec, u32 dwDisp, u32 dwFlags, HANDLE hTemplate) {
    std::string narrow = win_utf16_to_utf8(lpFileName);
    return hle_create_file_a(narrow.c_str(), dwAccess, dwShare, lpSec, dwDisp, dwFlags, hTemplate);
}

BOOL Win32ApiHle::hle_read_file(HANDLE hFile, void* lpBuffer, u32 nNumberOfBytesToRead, u32* lpNumberOfBytesRead, void* lpOverlapped) {
    int fd = static_cast<int>(reinterpret_cast<uintptr_t>(hFile));
    if (fd < 0 || !lpBuffer) return FALSE_VAL;

    u8* dst = static_cast<u8*>(lpBuffer);
    size_t total_read = 0;
    while (total_read < nNumberOfBytesToRead) {
        ssize_t n = read(fd, dst + total_read, nNumberOfBytesToRead - total_read);
        if (n > 0) {
            total_read += static_cast<size_t>(n);
        } else if (n == 0) {
            break; // EOF
        } else {
            if (errno == EINTR) continue;
            break;
        }
    }
    if (lpNumberOfBytesRead) *lpNumberOfBytesRead = static_cast<u32>(total_read);
    log::info("WIN32", "ReadFile fd={} count={} -> read={}", fd, nNumberOfBytesToRead, total_read);
    return TRUE_VAL;
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

// ---- Toolhelp32 snapshot (real /proc/self/task walk) ----
struct ToolhelpSnapshot {
    u32 tag{0x54484E53};          // "THNS"
    std::vector<pid_t> tids;
    size_t pos{0};
};
static std::unordered_map<void*, ToolhelpSnapshot*> g_toolhelp_snaps;
static std::mutex g_toolhelp_mtx;

BOOL Win32ApiHle::hle_close_handle(HANDLE hObject) {
    if (!hObject) return TRUE_VAL;
    {   // toolhelp snapshots carry their own tag
        std::lock_guard<std::mutex> lk(g_toolhelp_mtx);
        auto it = g_toolhelp_snaps.find(hObject);
        if (it != g_toolhelp_snaps.end()) { delete it->second; g_toolhelp_snaps.erase(it); return TRUE_VAL; }
    }
    switch (handle_tag(hObject)) {
        case kHandleThread: { delete static_cast<NativeThreadState*>(hObject); return TRUE_VAL; }
        case kHandleEvent:  { delete static_cast<NativeEventState*>(hObject); return TRUE_VAL; }
        case kHandleSem:    { delete static_cast<NativeSemaphoreState*>(hObject); return TRUE_VAL; }
        case kHandleMutex:  { delete static_cast<NativeMutexState*>(hObject); return TRUE_VAL; }
        case kHandleFind:   { return hle_find_close(hObject); }
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
    if (fd < 0) return 0xFFFFFFFF;
    int whence = SEEK_SET;
    if (dwMoveMethod == 1) whence = SEEK_CUR;
    else if (dwMoveMethod == 2) whence = SEEK_END;

    int64_t offset = 0;
    if (lpDistanceToMoveHigh) {
        uint64_t low = static_cast<uint32_t>(lDistanceToMove);
        uint64_t high = static_cast<uint32_t>(*lpDistanceToMoveHigh);
        offset = static_cast<int64_t>((high << 32) | low);
    } else {
        offset = static_cast<int64_t>(lDistanceToMove);
    }
    off_t res = lseek(fd, offset, whence);
    if (res == (off_t)-1) {
        g_last_error = 1;
        return 0xFFFFFFFF;
    }
    if (lpDistanceToMoveHigh) {
        *lpDistanceToMoveHigh = static_cast<s32>(static_cast<uint64_t>(res) >> 32);
    }
    log::info("WIN32", "SetFilePointer fd={} dist={} method={} -> res={}", fd, offset, dwMoveMethod, (u64)res);
    return static_cast<u32>(res & 0xFFFFFFFF);
}

BOOL Win32ApiHle::hle_set_file_pointer_ex(HANDLE hFile, int64_t liDistanceToMove, int64_t* lpNewFilePointer, u32 dwMoveMethod) {
    int fd = static_cast<int>(reinterpret_cast<uintptr_t>(hFile));
    if (fd < 0) return FALSE_VAL;
    int whence = SEEK_SET;
    if (dwMoveMethod == 1) whence = SEEK_CUR;
    else if (dwMoveMethod == 2) whence = SEEK_END;
    off_t res = lseek(fd, liDistanceToMove, whence);
    if (res == (off_t)-1) {
        g_last_error = 1;
        return FALSE_VAL;
    }
    if (lpNewFilePointer) *lpNewFilePointer = static_cast<int64_t>(res);
    log::info("WIN32", "SetFilePointerEx fd={} dist={} method={} -> res={}", fd, liDistanceToMove, dwMoveMethod, (u64)res);
    return TRUE_VAL;
}

u32 Win32ApiHle::hle_get_file_attributes_a(const char* lpFileName) {
    if (!lpFileName) {
        g_last_error = 2;
        return 0xFFFFFFFF;
    }
    std::string p;
    if (!pck_exe_fallback(lpFileName, p)) {
        g_last_error = 2;
        return 0xFFFFFFFF;
    }
    struct stat st{};
    if (stat(p.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
    }
    g_last_error = 2;
    return 0xFFFFFFFF;
}

u32 Win32ApiHle::hle_get_file_attributes_w(const wchar_t* lpFileName) {
    if (!lpFileName) {
        g_last_error = 2;
        return 0xFFFFFFFF;
    }
    std::string narrow = win_utf16_to_utf8(lpFileName);
    return hle_get_file_attributes_a(narrow.c_str());
}

BOOL Win32ApiHle::hle_get_file_attributes_ex_a(const char* lpFileName, int fInfoLevelId, void* lpFileInformation) {
    if (!lpFileName || !lpFileInformation) {
        g_last_error = 87; // ERROR_INVALID_PARAMETER
        return FALSE_VAL;
    }
    std::string p = normalize_win_path(lpFileName);
    struct stat st{};
    if (stat(p.c_str(), &st) != 0) {
        g_last_error = 2; // ERROR_FILE_NOT_FOUND
        return FALSE_VAL;
    }
    auto* data = static_cast<Win32FileAttributeData*>(lpFileInformation);
    std::memset(data, 0, sizeof(Win32FileAttributeData));
    data->dwFileAttributes = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
    data->nFileSizeLow = static_cast<u32>(st.st_size & 0xFFFFFFFF);
    data->nFileSizeHigh = static_cast<u32>(st.st_size >> 32);
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_get_file_attributes_ex_w(const wchar_t* lpFileName, int fInfoLevelId, void* lpFileInformation) {
    if (!lpFileName) return FALSE_VAL;
    std::string narrow = win_utf16_to_utf8(lpFileName);
    return hle_get_file_attributes_ex_a(narrow.c_str(), fInfoLevelId, lpFileInformation);
}

u32 Win32ApiHle::hle_get_full_path_name_a(const char* lpFileName, u32 nBufferLength, char* lpBuffer, char** lpFilePart) {
    if (!lpFileName) return 0;
    std::string p = normalize_win_path(lpFileName);
    if (lpBuffer && nBufferLength > 0) {
        std::strncpy(lpBuffer, p.c_str(), nBufferLength - 1);
        lpBuffer[nBufferLength - 1] = 0;
        if (lpFilePart) {
            char* last_slash = std::strrchr(lpBuffer, '/');
            *lpFilePart = last_slash ? (last_slash + 1) : lpBuffer;
        }
    }
    return static_cast<u32>(p.size());
}

u32 Win32ApiHle::hle_get_full_path_name_w(const wchar_t* lpFileName, u32 nBufferLength, wchar_t* lpBuffer, wchar_t** lpFilePart) {
    if (!lpFileName) return 0;
    std::string narrow = win_utf16_to_utf8(lpFileName);
    std::string p = normalize_win_path(narrow.c_str());
    if (lpBuffer && nBufferLength > 0) {
        uint16_t* dst = reinterpret_cast<uint16_t*>(lpBuffer);
        u32 len = 0;
        while (len < p.size() && len + 1 < nBufferLength) {
            dst[len] = static_cast<uint16_t>(p[len]);
            len++;
        }
        dst[len] = 0;
        if (lpFilePart) {
            wchar_t* last_slash = nullptr;
            for (u32 i = 0; i < len; ++i) {
                if (lpBuffer[i] == L'/' || lpBuffer[i] == L'\\') last_slash = &lpBuffer[i + 1];
            }
            *lpFilePart = last_slash ? last_slash : lpBuffer;
        }
    }
    return static_cast<u32>(p.size());
}

u32 Win32ApiHle::hle_get_current_directory_a(u32 nBufferLength, char* lpBuffer) {
    char buf[1024]{};
    if (getcwd(buf, sizeof(buf))) {
        u32 len = static_cast<u32>(std::strlen(buf));
        if (lpBuffer && nBufferLength > len) {
            std::strncpy(lpBuffer, buf, nBufferLength);
            return len;
        }
        return len + 1;
    }
    return 0;
}

u32 Win32ApiHle::hle_get_current_directory_w(u32 nBufferLength, wchar_t* lpBuffer) {
    char buf[1024]{};
    if (getcwd(buf, sizeof(buf))) {
        u32 len = static_cast<u32>(std::strlen(buf));
        if (lpBuffer && nBufferLength > len) {
            uint16_t* dst = reinterpret_cast<uint16_t*>(lpBuffer);
            for (u32 i = 0; i < len; ++i) dst[i] = static_cast<uint16_t>(buf[i]);
            dst[len] = 0;
            return len;
        }
        return len + 1;
    }
    return 0;
}

BOOL Win32ApiHle::hle_set_current_directory_a(const char* lpPathName) {
    if (!lpPathName) return FALSE_VAL;
    std::string p = normalize_win_path(lpPathName);
    return (chdir(p.c_str()) == 0) ? TRUE_VAL : FALSE_VAL;
}

BOOL Win32ApiHle::hle_set_current_directory_w(const wchar_t* lpPathName) {
    if (!lpPathName) return FALSE_VAL;
    std::string narrow = win_utf16_to_utf8(lpPathName);
    return hle_set_current_directory_a(narrow.c_str());
}

BOOL Win32ApiHle::hle_get_file_size_ex(HANDLE hFile, int64_t* lpFileSize) {
    int fd = static_cast<int>(reinterpret_cast<uintptr_t>(hFile));
    if (fd < 0 || !lpFileSize) return FALSE_VAL;
    struct stat st{};
    if (fstat(fd, &st) == 0) {
        *lpFileSize = static_cast<int64_t>(st.st_size);
        return TRUE_VAL;
    }
    return FALSE_VAL;
}

BOOL Win32ApiHle::hle_create_directory_w(const wchar_t* lpPathName, void* lpSec) {
    if (!lpPathName) return FALSE_VAL;
    std::string narrow = win_utf16_to_utf8(lpPathName);
    std::string p = normalize_win_path(narrow.c_str());
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return !ec ? TRUE_VAL : FALSE_VAL;
}

BOOL Win32ApiHle::hle_delete_file_w(const wchar_t* lpFileName) {
    if (!lpFileName) return FALSE_VAL;
    std::string narrow = win_utf16_to_utf8(lpFileName);
    std::string p = normalize_win_path(narrow.c_str());
    return (unlink(p.c_str()) == 0) ? TRUE_VAL : FALSE_VAL;
}

BOOL Win32ApiHle::hle_get_file_information_by_handle(HANDLE hFile, void* lpFileInformation) {
    int fd = static_cast<int>(reinterpret_cast<uintptr_t>(hFile));
    if (fd < 0 || !lpFileInformation) return FALSE_VAL;
    struct stat st{};
    if (fstat(fd, &st) == 0) {
        struct Win32ByHandleFileInformation {
            u32 dwFileAttributes;
            u32 ftCreationTimeLow, ftCreationTimeHigh;
            u32 ftLastAccessTimeLow, ftLastAccessTimeHigh;
            u32 ftLastWriteTimeLow, ftLastWriteTimeHigh;
            u32 dwVolumeSerialNumber;
            u32 nFileSizeHigh;
            u32 nFileSizeLow;
            u32 nNumberOfLinks;
            u32 nFileIndexHigh;
            u32 nFileIndexLow;
        };
        auto* info = static_cast<Win32ByHandleFileInformation*>(lpFileInformation);
        std::memset(info, 0, sizeof(*info));
        info->dwFileAttributes = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
        info->nFileSizeLow = static_cast<u32>(st.st_size & 0xFFFFFFFF);
        info->nFileSizeHigh = static_cast<u32>(st.st_size >> 32);
        info->nNumberOfLinks = static_cast<u32>(st.st_nlink);
        return TRUE_VAL;
    }
    return FALSE_VAL;
}

u32 Win32ApiHle::hle_get_file_type(HANDLE hFile) {
    int fd = static_cast<int>(reinterpret_cast<uintptr_t>(hFile));
    if (fd == 0 || fd == 1 || fd == 2) return 2; // FILE_TYPE_CHAR
    struct stat st{};
    if (fstat(fd, &st) == 0) {
        if (S_ISCHR(st.st_mode)) return 2; // FILE_TYPE_CHAR
        if (S_ISFIFO(st.st_mode)) return 3; // FILE_TYPE_PIPE
        return 1; // FILE_TYPE_DISK
    }
    return 0; // FILE_TYPE_UNKNOWN
}

u16 Win32ApiHle::hle_get_user_default_ui_language() { return 0x0409; } // en-US
u32 Win32ApiHle::hle_get_user_default_lcid() { return 0x0409; }
int Win32ApiHle::hle_get_locale_info_ex(const wchar_t* lpLocaleName, u32 LCType, wchar_t* lpLCData, int cchData) {
    if (lpLCData && cchData > 0) lpLCData[0] = 0;
    return 1;
}
int Win32ApiHle::hle_lc_map_string_w(u32 Locale, u32 dwMapFlags, const wchar_t* lpSrcStr, int cchSrc, wchar_t* lpDestStr, int cchDest) {
    if (!lpSrcStr) return 0;
    int len = (cchSrc < 0) ? static_cast<int>(std::char_traits<wchar_t>::length(lpSrcStr) + 1) : cchSrc;
    if (lpDestStr && cchDest >= len) {
        std::memcpy(lpDestStr, lpSrcStr, len * sizeof(wchar_t));
        return len;
    }
    return len;
}
int Win32ApiHle::hle_compare_string_w(u32 Locale, u32 dwCmpFlags, const wchar_t* lpString1, int cchCount1, const wchar_t* lpString2, int cchCount2) {
    if (!lpString1 || !lpString2) return 2;
    int len1 = (cchCount1 < 0) ? static_cast<int>(std::char_traits<wchar_t>::length(lpString1)) : cchCount1;
    int len2 = (cchCount2 < 0) ? static_cast<int>(std::char_traits<wchar_t>::length(lpString2)) : cchCount2;
    int min_len = std::min(len1, len2);
    for (int i = 0; i < min_len; ++i) {
        if (lpString1[i] < lpString2[i]) return 1; // CSTR_LESS_THAN
        if (lpString1[i] > lpString2[i]) return 3; // CSTR_GREATER_THAN
    }
    if (len1 < len2) return 1;
    if (len1 > len2) return 3;
    return 2; // CSTR_EQUAL
}
int Win32ApiHle::hle_compare_string_ordinal(const wchar_t* lpString1, int cchCount1, const wchar_t* lpString2, int cchCount2, BOOL bIgnoreCase) {
    return hle_compare_string_w(0, 0, lpString1, cchCount1, lpString2, cchCount2);
}
int Win32ApiHle::hle_compare_file_time(const void* lpFileTime1, const void* lpFileTime2) {
    if (!lpFileTime1 || !lpFileTime2) return 0;
    const auto* t1 = static_cast<const uint64_t*>(lpFileTime1);
    const auto* t2 = static_cast<const uint64_t*>(lpFileTime2);
    if (*t1 < *t2) return -1;
    if (*t1 > *t2) return 1;
    return 0;
}
BOOL Win32ApiHle::hle_system_time_to_tz_specific_local_time(const void* lpTimeZoneInformation, const void* lpUniversalTime, void* lpLocalTime) {
    if (lpUniversalTime && lpLocalTime) std::memcpy(lpLocalTime, lpUniversalTime, 16);
    return TRUE_VAL;
}
u32 Win32ApiHle::hle_get_time_zone_information(void* lpTimeZoneInformation) {
    if (lpTimeZoneInformation) std::memset(lpTimeZoneInformation, 0, 172);
    return 0; // TIME_ZONE_ID_UNKNOWN
}
BOOL Win32ApiHle::hle_get_volume_information_w(const wchar_t* lpRootPathName, wchar_t* lpVolumeNameBuffer, u32 nVolumeNameSize, u32* lpVolumeSerialNumber, u32* lpMaximumComponentLength, u32* lpFileSystemFlags, wchar_t* lpFileSystemNameBuffer, u32 nFileSystemNameSize) {
    if (lpVolumeSerialNumber) *lpVolumeSerialNumber = 0x12345678;
    if (lpMaximumComponentLength) *lpMaximumComponentLength = 255;
    if (lpFileSystemFlags) *lpFileSystemFlags = 0x3; // FILE_CASE_PRESERVED_NAMES | FILE_CASE_SENSITIVE_SEARCH
    if (lpFileSystemNameBuffer && nFileSystemNameSize >= 4) {
        uint16_t* dst = reinterpret_cast<uint16_t*>(lpFileSystemNameBuffer);
        dst[0] = 'N'; dst[1] = 'T'; dst[2] = 'F'; dst[3] = 'S'; dst[4] = 0;
    }
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_get_disk_free_space_ex_a(const char* lpDirectoryName, uint64_t* lpFreeBytesAvailableToCaller, uint64_t* lpTotalNumberOfBytes, uint64_t* lpTotalNumberOfFreeBytes) {
    if (lpFreeBytesAvailableToCaller) *lpFreeBytesAvailableToCaller = 100ULL * 1024 * 1024 * 1024;
    if (lpTotalNumberOfBytes) *lpTotalNumberOfBytes = 500ULL * 1024 * 1024 * 1024;
    if (lpTotalNumberOfFreeBytes) *lpTotalNumberOfFreeBytes = 100ULL * 1024 * 1024 * 1024;
    return TRUE_VAL;
}

// ---- real host-backed kernel32/ole glue (evidenced by Unity/Godot titles) ----
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <utime.h>

static void statvfs_for_path(const char* path, uint64_t* free_call, uint64_t* total, uint64_t* free_drive) {
    struct statvfs v{};
    const char* real = path ? normalize_win_path(path).c_str() : ".";
    if (statvfs(real, &v) != 0) { statvfs(".", &v); }
    uint64_t bsz = v.f_bsize;
    if (total) *total = v.f_blocks * bsz;
    if (free_drive) *free_drive = v.f_bavail * bsz;
    if (free_call) *free_call = v.f_bavail * bsz;
}

BOOL Win32ApiHle::hle_get_disk_free_space_ex_w(const wchar_t* lpDirectoryName, uint64_t* lpFreeBytesAvailableToCaller, uint64_t* lpTotalNumberOfBytes, uint64_t* lpTotalNumberOfFreeBytes) {
    std::string p = lpDirectoryName ? win_utf16_to_utf8(lpDirectoryName) : ".";
    statvfs_for_path(p.c_str(), lpFreeBytesAvailableToCaller, lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
    return TRUE_VAL;
}

u32 Win32ApiHle::hle_expand_environment_strings_a(const char* lpSrc, char* lpDst, u32 nSize) {
    if (!lpSrc) return 0;
    std::string out;
    const char* p = lpSrc;
    while (*p) {
        if (*p == '%') {
            const char* e = std::strchr(p + 1, '%');
            if (e) {
                std::string var(p + 1, e);
                const char* val = std::getenv(var.c_str());
                if (val) out += val;
                p = e + 1;
                continue;
            }
        }
        out += *p++;
    }
    if (lpDst && nSize) { std::strncpy(lpDst, out.c_str(), nSize - 1); lpDst[nSize - 1] = 0; }
    return static_cast<u32>(out.size() + 1);
}

u32 Win32ApiHle::hle_expand_environment_strings_w(const wchar_t* lpSrc, wchar_t* lpDst, u32 nSize) {
    if (!lpSrc) return 0;
    std::string narrow = win_utf16_to_utf8(lpSrc);
    int needed = hle_expand_environment_strings_a(narrow.c_str(), nullptr, 0);
    if (!lpDst || !nSize) return needed > 0 ? static_cast<u32>(needed) : 0;
    std::string tmp(static_cast<size_t>(needed), '\0');
    hle_expand_environment_strings_a(narrow.c_str(), tmp.data(), static_cast<u32>(needed));
    u32 n = std::min<u32>(static_cast<u32>(needed), nSize);
    for (u32 i = 0; i + 1 < n; ++i) lpDst[i] = static_cast<wchar_t>(static_cast<u8>(tmp[i]));
    if (n > 0) lpDst[n - 1] = 0;
    return static_cast<u32>(tmp.size());
}

BOOL Win32ApiHle::hle_set_file_attributes_a(const char* lpFileName, u32 dwFileAttributes) {
    if (!lpFileName) return FALSE_VAL;
    std::string p = normalize_win_path(lpFileName);
    struct stat st{};
    if (stat(p.c_str(), &st) != 0) return FALSE_VAL;
    mode_t m = st.st_mode;
    if (dwFileAttributes & 0x1) m &= ~(S_IWUSR | S_IWGRP | S_IWOTH);   // FILE_ATTRIBUTE_READONLY
    else m |= S_IWUSR;
    chmod(p.c_str(), m);
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_set_file_attributes_w(const wchar_t* lpFileName, u32 dwFileAttributes) {
    if (!lpFileName) return FALSE_VAL;
    std::string narrow = win_utf16_to_utf8(lpFileName);
    return hle_set_file_attributes_a(narrow.c_str(), dwFileAttributes);
}

BOOL Win32ApiHle::hle_set_file_time(HANDLE hFile, const void* lpCreation, const void* lpLastAccess, const void* lpLastWrite) {
    (void)lpCreation;
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(hFile));
    struct timespec times[2]{};
    // FILETIME: 100ns since 1601. Convert to unix seconds.
    auto ft_to_timespec = [](const void* ft) -> struct timespec {
        struct timespec ts{0, 0};
        if (!ft) return ts;
        u64 f = *static_cast<const u64*>(ft);
        if (f == 0) return ts;
        constexpr u64 kUnixEpochDelta = 116444736000000000ULL;
        u64 us100 = f > kUnixEpochDelta ? f - kUnixEpochDelta : 0;
        ts.tv_sec = static_cast<time_t>(us100 / 10000000);
        ts.tv_nsec = static_cast<long>((us100 % 10000000) * 100);
        return ts;
    };
    times[0] = ft_to_timespec(lpLastAccess);
    times[1] = ft_to_timespec(lpLastWrite);
    return (futimens(fd, times) == 0) ? TRUE_VAL : FALSE_VAL;
}

u32 Win32ApiHle::hle_suspend_thread(HANDLE hThread) {
    auto* nts = static_cast<NativeThreadState*>(hThread);
    if (!nts || nts->tag != kHandleThread) return static_cast<u32>(-1);
    return (pthread_kill(nts->handle, SIGSTOP) == 0) ? 1 : static_cast<u32>(-1);
}

u32 Win32ApiHle::hle_resume_thread(HANDLE hThread) {
    auto* nts = static_cast<NativeThreadState*>(hThread);
    if (!nts || nts->tag != kHandleThread) return static_cast<u32>(-1);
    return (pthread_kill(nts->handle, SIGCONT) == 0) ? 1 : static_cast<u32>(-1);
}

HANDLE Win32ApiHle::hle_create_toolhelp32_snapshot(u32 dwFlags, u32 th32ProcessID) {
    (void)dwFlags; (void)th32ProcessID;
    auto* sn = new ToolhelpSnapshot();
    for (const auto& de : std::filesystem::directory_iterator("/proc/self/task")) {
        try { sn->tids.push_back(static_cast<pid_t>(std::stoul(de.path().filename().string()))); }
        catch (...) {}
    }
    std::lock_guard<std::mutex> lk(g_toolhelp_mtx);
    g_toolhelp_snaps[sn] = sn;
    return sn;
}

BOOL Win32ApiHle::hle_thread32_first(HANDLE hSnapshot, void* lpte) {
    auto* sn = static_cast<ToolhelpSnapshot*>(hSnapshot);
    auto* e = static_cast<u8*>(lpte);
    if (!sn || !e) return FALSE_VAL;
    std::lock_guard<std::mutex> lk(g_toolhelp_mtx);
    if (sn->tids.empty()) return FALSE_VAL;
    u32 dwSize = *reinterpret_cast<u32*>(e);
    if (dwSize < 32) return FALSE_VAL;
    sn->pos = 0;
    *reinterpret_cast<u32*>(e + 8) = sn->tids[0];        // th32ThreadID
    *reinterpret_cast<u32*>(e + 12) = getpid();          // th32OwnerProcessID
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_thread32_next(HANDLE hSnapshot, void* lpte) {
    auto* sn = static_cast<ToolhelpSnapshot*>(hSnapshot);
    auto* e = static_cast<u8*>(lpte);
    if (!sn || !e) return FALSE_VAL;
    std::lock_guard<std::mutex> lk(g_toolhelp_mtx);
    if (sn->pos + 1 >= sn->tids.size()) return FALSE_VAL;
    ++sn->pos;
    *reinterpret_cast<u32*>(e + 8) = sn->tids[sn->pos];
    *reinterpret_cast<u32*>(e + 12) = getpid();
    return TRUE_VAL;
}

// ---- OLE glue ---------------------------------------------------------------
s32 Win32ApiHle::hle_ole_initialize(const void* pvReserved) {
    (void)pvReserved;
    return hle_co_initialize_ex(nullptr, 0);   // S_OK / S_FALSE mirror
}
void Win32ApiHle::hle_ole_uninitialize() {}

s32 Win32ApiHle::hle_register_drag_drop(void* hwnd, void* pDropTarget) { (void)hwnd; (void)pDropTarget; return 0x80004001; } // E_NOTIMPL
s32 Win32ApiHle::hle_revoke_drag_drop(void* hwnd) { (void)hwnd; return 0x80004001; }
s32 Win32ApiHle::hle_set_error_info(u32 dwReserved, void* perrinfo) { (void)dwReserved; (void)perrinfo; return 0; } // accept & drop
s32 Win32ApiHle::hle_get_error_info(u32 dwReserved, void** pperrinfo) { (void)dwReserved; if (pperrinfo) *pperrinfo = nullptr; return 0x80004001; }
s32 Win32ApiHle::hle_create_error_info(u32 dwReserved, void** pperrinfo) { (void)dwReserved; if (pperrinfo) *pperrinfo = nullptr; return 0x80004001; }
s32 Win32ApiHle::hle_co_create_free_threaded_marshaler(void* pOuter, void** ppMarshal) { (void)pOuter; if (ppMarshal) *ppMarshal = nullptr; return 0x80004001; }

void Win32ApiHle::hle_release_stg_medium(void* pmedium) {
    if (!pmedium) return;
    // STGMEDIUM: tymed@0, union@8, pUnkForRelease@16
    u32 tymed = *reinterpret_cast<u32*>(static_cast<u8*>(pmedium));
    void* unk = *reinterpret_cast<void**>(static_cast<u8*>(pmedium) + 16);
    void* data = *reinterpret_cast<void**>(static_cast<u8*>(pmedium) + 8);
    if (unk) { hle_com_release(unk); }
    else if (tymed == 1 && data) { (void)hle_global_free(data); }   // TYMED_HGLOBAL
    std::memset(pmedium, 0, 24);
}

u32 Win32ApiHle::hle_com_release(void* pUnk) {
    // IUnknown::Release via vtable slot 2
    if (!pUnk || !*reinterpret_cast<void**>(pUnk)) return 0;
    auto fn = reinterpret_cast<u32 (__attribute__((ms_abi))*)(void*)>((*reinterpret_cast<void***>(pUnk))[2]);
    return fn ? fn(pUnk) : 0;
}

u32 Win32ApiHle::hle_get_logical_drives() { return 0x4; } // Drive C:
u32 Win32ApiHle::hle_get_temp_file_name_w(const wchar_t* lpPathName, const wchar_t* lpPrefixString, u32 uUnique, wchar_t* lpTempFileName) {
    if (!lpTempFileName) return 0;
    const wchar_t* name = L"C:\\temp.tmp";
    std::memcpy(lpTempFileName, name, 12 * sizeof(wchar_t));
    return 1;
}
BOOL Win32ApiHle::hle_replace_file_w(const wchar_t* lpReplacedFileName, const wchar_t* lpReplacementFileName, const wchar_t* lpBackupFileName, u32 dwReplaceFlags, void* lpExclude, void* lpReserved) {
    if (!lpReplacedFileName || !lpReplacementFileName) return FALSE_VAL;
    std::string rep = normalize_win_path(win_utf16_to_utf8(lpReplacedFileName).c_str());
    std::string src = normalize_win_path(win_utf16_to_utf8(lpReplacementFileName).c_str());
    return (rename(src.c_str(), rep.c_str()) == 0) ? TRUE_VAL : FALSE_VAL;
}
BOOL Win32ApiHle::hle_move_file_ex_w(const wchar_t* lpExistingFileName, const wchar_t* lpNewFileName, u32 dwFlags) {
    if (!lpExistingFileName || !lpNewFileName) return FALSE_VAL;
    std::string src = normalize_win_path(win_utf16_to_utf8(lpExistingFileName).c_str());
    std::string dst = normalize_win_path(win_utf16_to_utf8(lpNewFileName).c_str());
    return (rename(src.c_str(), dst.c_str()) == 0) ? TRUE_VAL : FALSE_VAL;
}
BOOL Win32ApiHle::hle_remove_directory_w(const wchar_t* lpPathName) {
    if (!lpPathName) return FALSE_VAL;
    std::string p = normalize_win_path(win_utf16_to_utf8(lpPathName).c_str());
    return (rmdir(p.c_str()) == 0) ? TRUE_VAL : FALSE_VAL;
}
u32 Win32ApiHle::hle_get_drive_type_w(const wchar_t* lpRootPathName) { return 3; } // DRIVE_FIXED
HANDLE Win32ApiHle::hle_get_std_handle(u32 nStdHandle) {
    if (nStdHandle == static_cast<u32>(-10)) return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0)); // STD_INPUT_HANDLE
    if (nStdHandle == static_cast<u32>(-11)) return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1)); // STD_OUTPUT_HANDLE
    if (nStdHandle == static_cast<u32>(-12)) return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(2)); // STD_ERROR_HANDLE
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1));
}
BOOL Win32ApiHle::hle_set_std_handle(u32 nStdHandle, HANDLE hHandle) { return TRUE_VAL; }
s32 Win32ApiHle::hle_unhandled_exception_filter(void* ExceptionInfo) { return 1; } // EXCEPTION_EXECUTE_HANDLER
BOOL Win32ApiHle::hle_set_end_of_file(HANDLE hFile) {
    int fd = static_cast<int>(reinterpret_cast<uintptr_t>(hFile));
    if (fd < 0) return FALSE_VAL;
    off_t pos = lseek(fd, 0, SEEK_CUR);
    if (pos != (off_t)-1) return (ftruncate(fd, pos) == 0) ? TRUE_VAL : FALSE_VAL;
    return FALSE_VAL;
}
BOOL Win32ApiHle::hle_flush_file_buffers(HANDLE hFile) {
    int fd = static_cast<int>(reinterpret_cast<uintptr_t>(hFile));
    if (fd >= 0) fsync(fd);
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_peek_named_pipe(HANDLE hNamedPipe, void* lpBuffer, u32 nBufferSize, u32* lpBytesRead, u32* lpTotalBytesAvail, u32* lpBytesLeftThisMessage) {
    if (lpTotalBytesAvail) *lpTotalBytesAvail = 0;
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_create_pipe(HANDLE* hReadPipe, HANDLE* hWritePipe, void* lpPipeAttributes, u32 nSize) {
    int fds[2];
    if (pipe(fds) == 0) {
        if (hReadPipe) *hReadPipe = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fds[0]));
        if (hWritePipe) *hWritePipe = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fds[1]));
        return TRUE_VAL;
    }
    return FALSE_VAL;
}
BOOL Win32ApiHle::hle_set_handle_information(HANDLE hObject, u32 dwMask, u32 dwFlags) { return TRUE_VAL; }
BOOL Win32ApiHle::hle_get_exit_code_process(HANDLE hProcess, u32* lpExitCode) {
    if (lpExitCode) *lpExitCode = 0;
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_get_exit_code_thread(HANDLE hThread, u32* lpExitCode) {
    if (lpExitCode) *lpExitCode = 0;
    return TRUE_VAL;
}
HANDLE Win32ApiHle::hle_open_process(u32 dwDesiredAccess, BOOL bInheritHandle, u32 dwProcessId) {
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x7FFF00000001));
}
BOOL Win32ApiHle::hle_create_process_w(const wchar_t* lpApp, wchar_t* lpCmd, void* lpPA, void* lpTA, BOOL bInherit, u32 dwFlags, void* lpEnv, const wchar_t* lpCurrDir, void* lpSI, void* lpPI) {
    return FALSE_VAL;
}
static wchar_t g_empty_env[] = { 0, 0 };
wchar_t* Win32ApiHle::hle_get_environment_strings_w() { return g_empty_env; }
BOOL Win32ApiHle::hle_free_environment_strings_w(wchar_t* penv) { return TRUE_VAL; }
BOOL Win32ApiHle::hle_set_environment_variable_w(const wchar_t* lpName, const wchar_t* lpValue) { return TRUE_VAL; }
u32 Win32ApiHle::hle_get_environment_variable_w(const wchar_t* lpName, wchar_t* lpBuffer, u32 nSize) { return 0; }
u32 Win32ApiHle::hle_get_oemcp() { return 65001; } // UTF-8
BOOL Win32ApiHle::hle_is_valid_code_page(u32 CodePage) { return TRUE_VAL; }
BOOL Win32ApiHle::hle_is_valid_locale(u32 Locale, u32 dwFlags) { return TRUE_VAL; }
BOOL Win32ApiHle::hle_enum_system_locales_w(void* lpLocaleEnumProc, u32 dwFlags) { return TRUE_VAL; }
BOOL Win32ApiHle::hle_get_string_type_w(u32 dwInfoType, const wchar_t* lpSrcStr, int cchSrc, u16* lpCharType) {
    if (lpCharType && cchSrc > 0) std::memset(lpCharType, 0, cchSrc * sizeof(u16));
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_get_cpinfo(u32 CodePage, void* lpCPInfo) {
    if (lpCPInfo) std::memset(lpCPInfo, 0, 24);
    return TRUE_VAL;
}
u32 Win32ApiHle::hle_set_thread_ideal_processor(HANDLE hThread, u32 dwIdealProcessor) { return 0; }
uint64_t Win32ApiHle::hle_set_thread_affinity_mask(HANDLE hThread, uint64_t dwThreadAffinityMask) { return 1; }
BOOL Win32ApiHle::hle_set_thread_priority(HANDLE hThread, int nPriority) { return TRUE_VAL; }
BOOL Win32ApiHle::hle_set_priority_class(HANDLE hProcess, u32 dwPriorityClass) { return TRUE_VAL; }
HANDLE Win32ApiHle::hle_power_create_request(void* Context) { return reinterpret_cast<HANDLE>(0x7FFF00000002); }
BOOL Win32ApiHle::hle_power_set_request(HANDLE PowerRequest, u32 RequestType) { return TRUE_VAL; }
BOOL Win32ApiHle::hle_power_clear_request(HANDLE PowerRequest, u32 RequestType) { return TRUE_VAL; }
static std::mutex g_sync_mutex;
static std::unordered_map<void*, std::shared_mutex*> g_srw_locks;
static std::unordered_map<void*, std::condition_variable_any*> g_cond_vars;

static std::shared_mutex* get_srw_lock(void* ptr) {
    std::lock_guard<std::mutex> lock(g_sync_mutex);
    auto it = g_srw_locks.find(ptr);
    if (it != g_srw_locks.end()) return it->second;
    auto* mtx = new std::shared_mutex();
    g_srw_locks[ptr] = mtx;
    return mtx;
}

static std::condition_variable_any* get_cond_var(void* ptr) {
    std::lock_guard<std::mutex> lock(g_sync_mutex);
    auto it = g_cond_vars.find(ptr);
    if (it != g_cond_vars.end()) return it->second;
    auto* cv = new std::condition_variable_any();
    g_cond_vars[ptr] = cv;
    return cv;
}

void Win32ApiHle::hle_initialize_srw_lock(void* SRWLock) { if (SRWLock) *static_cast<void**>(SRWLock) = nullptr; }
void Win32ApiHle::hle_acquire_srw_lock_exclusive(void* SRWLock) { get_srw_lock(SRWLock)->lock(); }
void Win32ApiHle::hle_release_srw_lock_exclusive(void* SRWLock) { get_srw_lock(SRWLock)->unlock(); }
BOOL Win32ApiHle::hle_try_acquire_srw_lock_exclusive(void* SRWLock) { return get_srw_lock(SRWLock)->try_lock() ? TRUE_VAL : FALSE_VAL; }
void Win32ApiHle::hle_acquire_srw_lock_shared(void* SRWLock) { get_srw_lock(SRWLock)->lock_shared(); }
void Win32ApiHle::hle_release_srw_lock_shared(void* SRWLock) { get_srw_lock(SRWLock)->unlock_shared(); }

void Win32ApiHle::hle_initialize_condition_variable(void* ConditionVariable) { if (ConditionVariable) *static_cast<void**>(ConditionVariable) = nullptr; }
void Win32ApiHle::hle_wake_condition_variable(void* ConditionVariable) { get_cond_var(ConditionVariable)->notify_one(); }
void Win32ApiHle::hle_wake_all_condition_variable(void* ConditionVariable) { get_cond_var(ConditionVariable)->notify_all(); }

BOOL Win32ApiHle::hle_sleep_condition_variable_cs(void* ConditionVariable, Win32CriticalSection* CriticalSection, u32 dwMilliseconds) {
    if (!ConditionVariable || !CriticalSection || !CriticalSection->DebugInfo) return FALSE_VAL;
    auto* mtx = static_cast<std::recursive_mutex*>(CriticalSection->DebugInfo);
    auto* cv = get_cond_var(ConditionVariable);
    if (dwMilliseconds == 0xFFFFFFFF) {
        cv->wait(*mtx);
        return TRUE_VAL;
    } else {
        return cv->wait_for(*mtx, std::chrono::milliseconds(dwMilliseconds)) == std::cv_status::no_timeout ? TRUE_VAL : FALSE_VAL;
    }
}

BOOL Win32ApiHle::hle_sleep_condition_variable_srw(void* ConditionVariable, void* SRWLock, u32 dwMilliseconds, u32 Flags) {
    if (!ConditionVariable || !SRWLock) return FALSE_VAL;
    auto* mtx = get_srw_lock(SRWLock);
    auto* cv = get_cond_var(ConditionVariable);
    // Flags & 1 means shared mode (CONDITION_VARIABLE_LOCKMODE_SHARED)
    if (dwMilliseconds == 0xFFFFFFFF) {
        if (Flags & 1) {
            // C++ doesn't natively support waiting on a condition variable with a shared lock via std::unique_lock.
            // But std::condition_variable_any can take ANY Lockable!
            // We just need a wrapper for shared lock.
            struct SharedLockAdapter {
                std::shared_mutex& m;
                void lock() { m.lock_shared(); }
                void unlock() { m.unlock_shared(); }
            } adapter{*mtx};
            cv->wait(adapter);
        } else {
            struct ExclusiveLockAdapter {
                std::shared_mutex& m;
                void lock() { m.lock(); }
                void unlock() { m.unlock(); }
            } adapter{*mtx};
            cv->wait(adapter);
        }
        return TRUE_VAL;
    } else {
        bool res = false;
        if (Flags & 1) {
            struct SharedLockAdapter {
                std::shared_mutex& m;
                void lock() { m.lock_shared(); }
                void unlock() { m.unlock_shared(); }
            } adapter{*mtx};
            res = cv->wait_for(adapter, std::chrono::milliseconds(dwMilliseconds)) == std::cv_status::no_timeout;
        } else {
            struct ExclusiveLockAdapter {
                std::shared_mutex& m;
                void lock() { m.lock(); }
                void unlock() { m.unlock(); }
            } adapter{*mtx};
            res = cv->wait_for(adapter, std::chrono::milliseconds(dwMilliseconds)) == std::cv_status::no_timeout;
        }
        return res ? TRUE_VAL : FALSE_VAL;
    }
}

BOOL Win32ApiHle::hle_init_once_begin_initialize(void* InitOnce, u32 dwFlags, BOOL* fPending, void** lpContext) {
    // Basic stub that just assumes already initialized to prevent blocking, but maybe we should implement it?
    // Let's implement a very basic one:
    // InitOnce is a PVOID. 0 = uninitialized, 1 = initializing, 2 = done.
    if (!InitOnce) return FALSE_VAL;
    auto* state = static_cast<std::atomic<u64>*>(InitOnce);
    u64 val = 0;
    if (state->compare_exchange_strong(val, 1)) {
        if (fPending) *fPending = TRUE_VAL;
        return TRUE_VAL;
    }
    // Spin until it is 2
    while (state->load() == 1) {
        std::this_thread::yield();
    }
    if (fPending) *fPending = FALSE_VAL;
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_init_once_complete(void* InitOnce, u32 dwFlags, void* lpContext) {
    if (!InitOnce) return FALSE_VAL;
    auto* state = static_cast<std::atomic<u64>*>(InitOnce);
    state->store(2);
    return TRUE_VAL;
}
void Win32ApiHle::hle_initialize_slist_head(void* ListHead) { if (ListHead) std::memset(ListHead, 0, 16); }
void* Win32ApiHle::hle_interlocked_push_entry_slist(void* ListHead, void* ListEntry) { return nullptr; }
void* Win32ApiHle::hle_global_alloc(u32 uFlags, size_t uBytes) { return hle_heap_alloc(hle_get_process_heap(), 0, uBytes); }
void* Win32ApiHle::hle_global_lock(void* hMem) { return hMem; }
BOOL Win32ApiHle::hle_global_unlock(void* hMem) { return TRUE_VAL; }

// ---- Memory / string / locale helpers (added by Hermes) ------------------------
void* Win32ApiHle::hle_global_free(void* hMem) {
    if (hMem) hle_heap_free(hle_get_process_heap(), 0, hMem);
    return nullptr;
}
char* Win32ApiHle::hle_lstrcat_a(char* dst, const char* src) {
    if (dst && src) std::strcat(dst, src);
    return dst;
}
int Win32ApiHle::hle_lstrlen_a(const char* str) { return str ? static_cast<int>(std::strlen(str)) : 0; }
u32 Win32ApiHle::hle_wcslen(const void* str) {
    if (!str) return 0;
    // Guest wchar_t is 16-bit (UTF-16 units); count them directly (host
    // wchar_t is 32-bit so std::wcslen would over-read).
    const u16* p = static_cast<const u16*>(str);
    u32 n = 0;
    while (p[n] != 0) ++n;
    return n;
}
u32 Win32ApiHle::hle_get_system_default_lang_id() { return 0x0409; }
u32 Win32ApiHle::hle_get_user_default_lang_id() { return 0x0409; }
u32 Win32ApiHle::hle_get_process_id(void* hProcess) {
    return hProcess ? static_cast<u32>(reinterpret_cast<uintptr_t>(hProcess) & 0xFFFF) : 1u;
}
u32 Win32ApiHle::hle_get_thread_locale(u32 dwFlags) { (void)dwFlags; return 0x0409; }
BOOL Win32ApiHle::hle_get_handle_information(void* hObject, u32* lpdwFlags) {
    (void)hObject;
    if (lpdwFlags) *lpdwFlags = 0;
    return TRUE_VAL;
}
void Win32ApiHle::hle_secure_zero_memory(void* pv, u64 cb) {
    if (pv && cb) {
        volatile u8* p = static_cast<volatile u8*>(pv);
        while (cb--) *p++ = 0;
    }
}

HANDLE Win32ApiHle::hle_find_first_file_a(const char* lpFileName, Win32FileFindDataA* lpFindFileData) {
    if (!lpFileName || !lpFindFileData) {
        g_last_error = 2;
        return reinterpret_cast<HANDLE>(reinterpret_cast<void*>(-1));
    }
    std::string norm = normalize_win_path(lpFileName);
    std::string dir_path = ".";
    std::string pattern = "*";
    size_t last_slash = norm.find_last_of('/');
    if (last_slash != std::string::npos) {
        dir_path = norm.substr(0, last_slash);
        pattern = norm.substr(last_slash + 1);
        if (dir_path.empty()) dir_path = "/";
    } else {
        pattern = norm;
    }
    if (pattern.empty() || pattern == "*.*") pattern = "*";

    DIR* d = opendir(dir_path.c_str());
    if (!d) {
        g_last_error = 2;
        return reinterpret_cast<HANDLE>(reinterpret_cast<void*>(-1));
    }

    auto* state = new NativeFindState();
    state->dir = d;
    state->base_dir = dir_path;
    state->pattern = pattern;

    if (hle_find_next_file_a(state, lpFindFileData)) {
        return state;
    }
    closedir(d);
    delete state;
    g_last_error = 2;
    return reinterpret_cast<HANDLE>(reinterpret_cast<void*>(-1));
}

BOOL Win32ApiHle::hle_find_next_file_a(HANDLE hFindFile, Win32FileFindDataA* lpFindFileData) {
    if (!hFindFile || handle_tag(hFindFile) != kHandleFind || !lpFindFileData) {
        g_last_error = 6; // ERROR_INVALID_HANDLE
        return FALSE_VAL;
    }
    auto* state = static_cast<NativeFindState*>(hFindFile);
    struct dirent* entry = nullptr;
    while ((entry = readdir(state->dir)) != nullptr) {
        if (state->pattern != "*" && state->pattern != "*.*" && fnmatch(state->pattern.c_str(), entry->d_name, 0) != 0) {
            continue;
        }
        std::memset(lpFindFileData, 0, sizeof(Win32FileFindDataA));
        std::strncpy(lpFindFileData->cFileName, entry->d_name, 259);
        std::string full = state->base_dir + "/" + entry->d_name;
        struct stat st{};
        if (stat(full.c_str(), &st) == 0) {
            lpFindFileData->dwFileAttributes = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
            lpFindFileData->nFileSizeLow = static_cast<u32>(st.st_size & 0xFFFFFFFF);
            lpFindFileData->nFileSizeHigh = static_cast<u32>(st.st_size >> 32);
        } else {
            lpFindFileData->dwFileAttributes = 0x80;
        }
        return TRUE_VAL;
    }
    g_last_error = 18; // ERROR_NO_MORE_FILES
    return FALSE_VAL;
}

HANDLE Win32ApiHle::hle_find_first_file_w(const wchar_t* lpFileName, Win32FileFindDataW* lpFindFileData) {
    if (!lpFileName || !lpFindFileData) {
        g_last_error = 2;
        return reinterpret_cast<HANDLE>(reinterpret_cast<void*>(-1));
    }
    std::string narrow = win_utf16_to_utf8(lpFileName);
    std::string p = normalize_win_path(narrow.c_str());
    std::string base_dir = ".";
    std::string pattern = "*";

    size_t last_slash = p.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        base_dir = p.substr(0, last_slash);
        pattern = p.substr(last_slash + 1);
    } else {
        pattern = p;
    }
    if (pattern.empty()) pattern = "*";

    DIR* d = opendir(base_dir.c_str());
    if (!d) {
        g_last_error = 2;
        return reinterpret_cast<HANDLE>(reinterpret_cast<void*>(-1));
    }

    auto* state = new NativeFindState();
    state->tag = kHandleFind;
    state->dir = d;
    state->base_dir = base_dir;
    state->pattern = pattern;

    if (hle_find_next_file_w(state, lpFindFileData)) {
        return state;
    }
    closedir(d);
    delete state;
    g_last_error = 2;
    return reinterpret_cast<HANDLE>(reinterpret_cast<void*>(-1));
}

HANDLE Win32ApiHle::hle_find_first_file_ex_w(const wchar_t* lpFileName, int fInfoLevelId, Win32FileFindDataW* lpFindFileData, int fSearchOp, void* lpSearchFilter, u32 dwAdditionalFlags) {
    return hle_find_first_file_w(lpFileName, lpFindFileData);
}

BOOL Win32ApiHle::hle_find_next_file_w(HANDLE hFindFile, Win32FileFindDataW* lpFindFileData) {
    if (!hFindFile || handle_tag(hFindFile) != kHandleFind || !lpFindFileData) {
        g_last_error = 6;
        return FALSE_VAL;
    }
    auto* state = static_cast<NativeFindState*>(hFindFile);
    struct dirent* entry = nullptr;
    while ((entry = readdir(state->dir)) != nullptr) {
        if (state->pattern != "*" && state->pattern != "*.*" && fnmatch(state->pattern.c_str(), entry->d_name, 0) != 0) {
            continue;
        }
        std::memset(lpFindFileData, 0, sizeof(Win32FileFindDataW));
        size_t len = 0;
        while (entry->d_name[len] && len + 1 < 260) {
            lpFindFileData->cFileName[len] = static_cast<uint16_t>(entry->d_name[len]);
            len++;
        }
        lpFindFileData->cFileName[len] = 0;
        std::string full = state->base_dir + "/" + entry->d_name;
        struct stat st{};
        if (stat(full.c_str(), &st) == 0) {
            lpFindFileData->dwFileAttributes = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
            lpFindFileData->nFileSizeLow = static_cast<u32>(st.st_size & 0xFFFFFFFF);
            lpFindFileData->nFileSizeHigh = static_cast<u32>(st.st_size >> 32);
        } else {
            lpFindFileData->dwFileAttributes = 0x80;
        }
        return TRUE_VAL;
    }
    g_last_error = 18; // ERROR_NO_MORE_FILES
    return FALSE_VAL;
}

BOOL Win32ApiHle::hle_find_close(HANDLE hFindFile) {
    if (!hFindFile || handle_tag(hFindFile) != kHandleFind) return FALSE_VAL;
    auto* state = static_cast<NativeFindState*>(hFindFile);
    if (state->dir) closedir(state->dir);
    delete state;
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
    log::info("WIN32", "GetProcAddress(hModule=0x{:X}, sym='{}')", reinterpret_cast<u64>(hModule), symbol_name);

    // 1. Resolve directly from guest PE module if passed and matches a loaded DLL
    auto* loader = PeLoader::active();
    if (loader && hModule != nullptr) {
        for (const auto& [name, img] : loader->loaded_dlls()) {
            if (img.image_base == hModule) {
                void* guest_sym = PeLoader::resolve_guest_export(hModule, symbol_name);
                if (guest_sym) return guest_sym;
                break;
            }
        }
    }

    // 2. Resolve from all loaded guest DLLs in active loader
    if (loader) {
        for (const auto& [name, img] : loader->loaded_dlls()) {
            void* guest_sym = PeLoader::resolve_guest_export(img.image_base, symbol_name);
            if (guest_sym) return guest_sym;
        }
    }

    // 3. Resolve from HLE export table
    if (g_active_win32_hle) {
        for (const auto& [dll, funcs] : g_active_win32_hle->export_table_) {
            auto it = funcs.find(symbol_name);
            if (it != funcs.end()) {
                return it->second;
            }
        }
    }

    if (symbol_name == "EncodePointer") return reinterpret_cast<void*>(&hle_encode_pointer);
    if (symbol_name == "DecodePointer") return reinterpret_cast<void*>(&hle_decode_pointer);
    if (symbol_name == "EncodeSystemPointer") return reinterpret_cast<void*>(&hle_encode_system_pointer);
    if (symbol_name == "DecodeSystemPointer") return reinterpret_cast<void*>(&hle_decode_system_pointer);
    if (symbol_name == "FlsAlloc") return reinterpret_cast<void*>(&hle_fls_alloc);
    if (symbol_name == "FlsFree") return reinterpret_cast<void*>(&hle_fls_free);
    if (symbol_name == "FlsGetValue") return reinterpret_cast<void*>(&hle_fls_get_value);
    if (symbol_name == "FlsSetValue") return reinterpret_cast<void*>(&hle_fls_set_value);
    if (symbol_name == "InitializeCriticalSectionEx") return reinterpret_cast<void*>(&hle_init_critical_section_ex);
    if (symbol_name == "InitializeCriticalSection") return reinterpret_cast<void*>(&hle_init_critical_section);
    if (symbol_name == "InitializeCriticalSectionAndSpinCount") return reinterpret_cast<void*>(&hle_init_critical_section_and_spin_count);
    if (symbol_name == "CreateEventExA" || symbol_name == "CreateEventA") return reinterpret_cast<void*>(&hle_create_event_a);
    if (symbol_name == "CreateEventExW" || symbol_name == "CreateEventW") return reinterpret_cast<void*>(&hle_create_event_w);
    if (symbol_name == "CreateSemaphoreExA" || symbol_name == "CreateSemaphoreA") return reinterpret_cast<void*>(&hle_create_semaphore_a);
    if (symbol_name == "CreateSemaphoreExW" || symbol_name == "CreateSemaphoreW") return reinterpret_cast<void*>(&hle_create_semaphore_w);
    if (symbol_name == "CreateMutexExA" || symbol_name == "CreateMutexA" || symbol_name == "CreateMutexW" || symbol_name == "CreateMutexExW") return reinterpret_cast<void*>(&hle_create_mutex_a);
    if (symbol_name == "GetFileAttributesExA") return reinterpret_cast<void*>(&hle_get_file_attributes_ex_a);
    if (symbol_name == "GetFileAttributesExW") return reinterpret_cast<void*>(&hle_get_file_attributes_ex_w);
    if (symbol_name == "FindFirstFileA") return reinterpret_cast<void*>(&hle_find_first_file_a);
    if (symbol_name == "FindFirstFileW") return reinterpret_cast<void*>(&hle_find_first_file_w);
    if (symbol_name == "FindFirstFileExW") return reinterpret_cast<void*>(&hle_find_first_file_ex_w);
    if (symbol_name == "FindNextFileA") return reinterpret_cast<void*>(&hle_find_next_file_a);
    if (symbol_name == "FindNextFileW") return reinterpret_cast<void*>(&hle_find_next_file_w);
    if (symbol_name == "FindClose") return reinterpret_cast<void*>(&hle_find_close);

    if (symbol_name == "SetProcessDpiAwareness") return reinterpret_cast<void*>(&generic_stub_zero);
    if (symbol_name == "SetProcessDpiAwarenessContext" || symbol_name == "SetProcessDPIAware") return reinterpret_cast<void*>(&generic_stub_arg0);
    if (symbol_name == "GetDpiForWindow" || symbol_name == "GetDpiForSystem") return reinterpret_cast<void*>(&hle_get_dpi_for_window);
    if (symbol_name == "GetDpiForMonitor") return reinterpret_cast<void*>(&hle_get_dpi_for_monitor);
    if (symbol_name == "EnableNonClientDpiScaling") return reinterpret_cast<void*>(&generic_stub_arg0);
    if (symbol_name == "LogicalToPhysicalPointForPerMonitorDPI") return reinterpret_cast<void*>(&generic_stub_arg0);
    if (symbol_name == "GetPointerType") return reinterpret_cast<void*>(&hle_get_pointer_type);

    if (symbol_name.starts_with("gl") || symbol_name.starts_with("wgl")) {
        void* p = hle_wgl_get_proc_address(symbol_name.c_str());
        if (p) return p;
    }
    if (symbol_name.starts_with("vk")) {
        void* p = hle_vk_get_instance_proc_addr(nullptr, symbol_name.c_str());
        if (p) return p;
    }

    g_last_error = 127;
    return nullptr;
}

void* Win32ApiHle::hle_get_module_handle_a(const char* lpModuleName) {
    if (!lpModuleName) return reinterpret_cast<void*>(0x140000000); // Main EXE handle
    std::string name(lpModuleName);
    for (auto& c : name) c = static_cast<char>(std::tolower(c));
    auto* loader = PeLoader::active();
    if (loader) {
        for (const auto& [n, img] : loader->loaded_dlls()) {
            std::string lower_n = n;
            for (auto& c : lower_n) c = static_cast<char>(std::tolower(c));
            if (lower_n.find(name) != std::string::npos || name.find(lower_n) != std::string::npos) {
                return img.image_base;
            }
        }
    }
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
    std::string narrow = win_utf16_to_utf8(lpModuleName);
    return hle_get_module_handle_a(narrow.c_str());
}

void* Win32ApiHle::hle_load_library_a(const char* lpLibFileName) {
    if (!lpLibFileName) return nullptr;
    std::string path = normalize_win_path(lpLibFileName);
    auto* loader = PeLoader::active();
    if (loader) {
        const auto& dlls = loader->loaded_dlls();
        auto it = dlls.find(lpLibFileName);
        if (it != dlls.end()) return it->second.image_base;
        it = dlls.find(path);
        if (it != dlls.end()) return it->second.image_base;

        if (std::filesystem::exists(path)) {
            auto res = loader->load_from_file(path);
            if (res.has_value()) {
                log::info("WIN32", "Loaded dynamic PE module '{}' at 0x{:X}", path,
                          reinterpret_cast<uintptr_t>(res->image_base));
                loader->add_loaded_dll(lpLibFileName, *res);
                loader->add_loaded_dll(path, *res);
                if (res->entry_point) {
                    using DllMainFn = BOOL(PAPAYA_MS_ABI*)(void*, u32, void*);
                    auto dll_main = reinterpret_cast<DllMainFn>(res->entry_point);
                    dll_main(res->image_base, 1 /* DLL_PROCESS_ATTACH */, nullptr);
                }
                return res->image_base;
            }
        }
    }
    return hle_get_module_handle_a(lpLibFileName);
}

void* Win32ApiHle::hle_load_library_w(const wchar_t* lpLibFileName) {
    if (!lpLibFileName) return nullptr;
    std::string narrow = win_utf16_to_utf8(lpLibFileName);
    return hle_load_library_a(narrow.c_str());
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

static std::string g_current_game_path = "game.exe";
static std::string g_current_game_name = "game.exe";
static std::string g_cmdline = "game.exe";
static std::vector<uint16_t> g_wcmdline = { 'g','a','m','e','.','e','x','e', 0 };

void Win32ApiHle::set_game_path(const std::string& path) {
    g_current_game_path = path;
    size_t last_slash = path.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        g_current_game_name = path.substr(last_slash + 1);
    } else {
        g_current_game_name = path;
    }
    g_cmdline = "\"" + g_current_game_name + "\"";
    g_wcmdline.clear();
    for (char c : g_cmdline) {
        g_wcmdline.push_back(static_cast<uint16_t>(c));
    }
    g_wcmdline.push_back(0);
}

u32 Win32ApiHle::hle_get_module_file_name_a(void* hModule, char* lpFilename, u32 nSize) {
    if (!lpFilename || nSize == 0) return 0;
    std::string p = "C:\\" + g_current_game_name;
    std::strncpy(lpFilename, p.c_str(), nSize - 1);
    lpFilename[nSize - 1] = '\0';
    return static_cast<u32>(std::strlen(lpFilename));
}

u32 Win32ApiHle::hle_get_module_file_name_w(void* hModule, wchar_t* lpFilename, u32 nSize) {
    if (!lpFilename || nSize == 0) return 0;
    std::string p = "C:\\" + g_current_game_name;
    uint16_t* dst = reinterpret_cast<uint16_t*>(lpFilename);
    u32 len = 0;
    while (len < p.size() && len + 1 < nSize) {
        dst[len] = static_cast<uint16_t>(p[len]);
        len++;
    }
    dst[len] = 0;
    return len;
}

static const u64 g_pointer_cookie = 0x2B992DD785DDULL;

void* Win32ApiHle::hle_encode_pointer(void* ptr) {
    if (!ptr) return nullptr;
    u64 val = reinterpret_cast<u64>(ptr);
    u8 shift = static_cast<u8>(g_pointer_cookie & 0x3F);
    val = (val << shift) | (val >> ((64 - shift) & 0x3F)); // rol
    val ^= g_pointer_cookie;
    return reinterpret_cast<void*>(val);
}

void* Win32ApiHle::hle_decode_pointer(void* ptr) {
    if (!ptr) return nullptr;
    u64 val = reinterpret_cast<u64>(ptr);
    u8 shift = static_cast<u8>(g_pointer_cookie & 0x3F);
    val ^= g_pointer_cookie;
    val = (val >> shift) | (val << ((64 - shift) & 0x3F)); // ror
    return reinterpret_cast<void*>(val);
}

void* Win32ApiHle::hle_encode_system_pointer(void* ptr) { return hle_encode_pointer(ptr); }
void* Win32ApiHle::hle_decode_system_pointer(void* ptr) { return hle_decode_pointer(ptr); }

u32 Win32ApiHle::hle_get_current_processor_number() {
    // As-if single-logical-CPU view (or map to sched_getcpu on Linux).
    return static_cast<u32>(sched_getcpu());
}
void* Win32ApiHle::hle_interlocked_flush_slist(void* head) {
    // Pop the whole thread-safe singly-linked list and return the first entry.
    // The HLE does not track an actual list; report empty.
    (void)head;
    return nullptr;
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
    switch (ProcessorFeature) {
    case 0:  // PF_FLOATING_POINT_PRECISION_ERRATA
    case 1:  // PF_FLOATING_POINT_EMULATED
    case 23: // PF_FASTFAIL_AVAILABLE (disable int 0x29 in Linux userspace)
    case 24: // PF_ARM_V8_INSTRUCTIONS_AVAILABLE
    case 25: // PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE
    case 26: // PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE
    case 27: // PF_CHPE_X86_ARM64
        return FALSE_VAL;
    case 2:  // PF_COMPARE_EXCHANGE_DOUBLE
    case 3:  // PF_MMX_INSTRUCTIONS_AVAILABLE
    case 6:  // PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE)
    case 7:  // PF_3DNOW_INSTRUCTIONS_AVAILABLE
    case 8:  // PF_RDTSC_INSTRUCTION_AVAILABLE
    case 9:  // PF_PAE_ENABLED
    case 10: // PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2)
    case 12: // PF_NX_ENABLED
    case 13: // PF_SSE3_INSTRUCTIONS_AVAILABLE
    case 14: // PF_COMPARE_EXCHANGE128
    case 17: // PF_AVX_INSTRUCTIONS_AVAILABLE
    case 18: // PF_AVX2_INSTRUCTIONS_AVAILABLE
    default:
        return TRUE_VAL;
    }
}

const char* Win32ApiHle::hle_get_command_line_a() { return g_cmdline.c_str(); }
const wchar_t* Win32ApiHle::hle_get_command_line_w() { return reinterpret_cast<const wchar_t*>(g_wcmdline.data()); }

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

char* Win32ApiHle::hle_lstrcpy_a(char* dst, const char* src) {
    if (!dst) return dst;
    if (src) std::strcpy(dst, src); else dst[0] = 0;
    return dst;
}
int Win32ApiHle::hle_lstrcmp_a(const char* a, const char* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return std::strcmp(a, b);
}
u32 Win32ApiHle::hle_get_thread_priority(void* hThread) {
    (void)hThread;
    return 0;   // THREAD_PRIORITY_NORMAL
}
u32 Win32ApiHle::hle_get_private_profile_string_a(const char* app, const char* key, const char* def,
                                                  char* out, u32 size, const char* file) {
    if (!out || size == 0) return 0;
    out[0] = 0;
    std::string path = normalize_win_path(file ? file : "");
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (def && size) std::strncpy(out, def, size - 1), out[size-1]=0; return static_cast<u32>(std::strlen(out)); }
    std::string line, section;
    while (std::getline(in, line)) {
        // strip trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // trim
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        size_t e = line.find_last_not_of(" \t");
        std::string t = line.substr(b, e - b + 1);
        if (t.front() == '[' && t.back() == ']') { section = t.substr(1, t.size() - 2); continue; }
        if (section != (app ? app : "")) continue;
        size_t eq = t.find('=');
        std::string k = (eq == std::string::npos) ? t : t.substr(0, eq);
        if (k == (key ? key : "")) {
            std::string v = (eq == std::string::npos) ? "" : t.substr(eq + 1);
            u32 n = static_cast<u32>(v.size());
            if (n >= size) n = size - 1;
            std::memcpy(out, v.c_str(), n); out[n] = 0;
            return n;
        }
    }
    if (def && size) { std::strncpy(out, def, size - 1); out[size-1] = 0; }
    return static_cast<u32>(std::strlen(out));
}
BOOL Win32ApiHle::hle_write_private_profile_string_a(const char* app, const char* key, const char* value, const char* file) {
    if (!app || !key || !file) return FALSE_VAL;
    std::string path = normalize_win_path(file);
    std::string target_app = "[" + std::string(app) + "]";
    std::string kv = std::string(key) + "=" + (value ? value : "");
    // Read the whole file, update or append section/key, write back.
    std::vector<std::string> lines;
    { std::ifstream in(path, std::ios::binary); std::string l;
      while (std::getline(in, l)) { if (!l.empty() && l.back()=='\r') l.pop_back(); lines.push_back(l); } }
    bool in_app = false, replaced = false, app_seen = false;
    for (auto& l : lines) {
        std::string t = l; size_t b = t.find_first_not_of(" \t");
        if (b != std::string::npos) t = t.substr(b);
        if (!t.empty() && t.front()=='[' && t.back()==']') { in_app = (t == target_app); if (in_app) app_seen = true; continue; }
        if (!in_app) continue;
        size_t eq = t.find('=');
        std::string k = (eq == std::string::npos) ? t : t.substr(0, eq);
        if (k == key) { l = kv; replaced = true; break; }
    }
    if (!replaced) {
        std::vector<std::string> nl;
        bool inserted = false;
        for (auto& l : lines) {
            nl.push_back(l);
            if (!inserted && l == target_app) { nl.push_back(kv); inserted = true; }
        }
        if (!inserted) { if (!nl.empty() && !nl.back().empty()) nl.push_back(""); nl.push_back(target_app); nl.push_back(kv); }
        lines.swap(nl);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return FALSE_VAL;
    for (auto& l : lines) out << l << '\n';
    return TRUE_VAL;
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

// Helper to convert wide or atom pointer to std::string
static std::string wide_or_atom_to_utf8(const void* ptr) {
    if (!ptr) return "";
    uintptr_t val = reinterpret_cast<uintptr_t>(ptr);
    if (val < 0x10000) {
        return "GodotEngine";
    }
    return win_utf16_to_utf8(ptr);
}

// ---- Window classes / creation ----
void* Win32ApiHle::hle_register_class_a(const void* lpWndClass) {
    if (!lpWndClass) return nullptr;
    const auto* buf = static_cast<const u8*>(lpWndClass);
    void* wndproc  = nullptr;
    std::memcpy(&wndproc, buf + 8, sizeof(wndproc));
    const char* cls_name = nullptr;
    std::memcpy(&cls_name, buf + 64, sizeof(cls_name));
    void* hinst = nullptr;
    std::memcpy(&hinst, buf + 24, sizeof(hinst));
    std::string cls_str = cls_name ? cls_name : "GodotEngine";
    log::info("WIN32", "RegisterClassA: name='{}', wndproc=0x{:X}", cls_str, reinterpret_cast<u64>(wndproc));
    window_manager().register_class(cls_str.c_str(), wndproc ? wndproc : reinterpret_cast<void*>(0x140001000), hinst);
    return reinterpret_cast<void*>(0xC001);
}

void* Win32ApiHle::hle_register_class_w(const void* lpWndClass) {
    if (!lpWndClass) return nullptr;
    const auto* buf = static_cast<const u8*>(lpWndClass);
    void* wndproc = nullptr;
    std::memcpy(&wndproc, buf + 8, sizeof(wndproc));
    const void* cls_name_ptr = nullptr;
    std::memcpy(&cls_name_ptr, buf + 64, sizeof(cls_name_ptr));
    void* hinst = nullptr;
    std::memcpy(&hinst, buf + 24, sizeof(hinst));
    std::string cls_str = wide_or_atom_to_utf8(cls_name_ptr);
    if (cls_str.empty()) cls_str = "GodotEngine";
    log::info("WIN32", "RegisterClassW: name='{}', wndproc=0x{:X}", cls_str, reinterpret_cast<u64>(wndproc));
    window_manager().register_class(cls_str.c_str(), wndproc ? wndproc : reinterpret_cast<void*>(0x140001000), hinst);
    return reinterpret_cast<void*>(0xC001);
}

void* Win32ApiHle::hle_create_window_ex_a(u32 dwExStyle, const char* lpClassName,
             const char* lpWindowName, u32 dwStyle, int x, int y, int w, int h,
             void* hWndParent, void* hMenu, void* hInstance, void* lpParam) {
    (void)dwExStyle; (void)hMenu; (void)lpParam;
    window_manager().initialize();
    return window_manager().create_window_ex(lpClassName, lpWindowName, dwStyle,
                                             x, y, w, h, hWndParent, hInstance, lpParam, false);
}

void* Win32ApiHle::hle_create_window_ex_w(u32 dwExStyle, const wchar_t* lpClassName,
             const wchar_t* lpWindowName, u32 dwStyle, int x, int y, int w, int h,
             void* hWndParent, void* hMenu, void* hInstance, void* lpParam) {
    (void)dwExStyle; (void)hMenu; (void)lpParam;
    std::string cls_str = wide_or_atom_to_utf8(lpClassName);
    std::string win_str = wide_or_atom_to_utf8(lpWindowName);
    log::info("WIN32", "CreateWindowExW: cls='{}' title='{}' size={}x{}", cls_str, win_str, w, h);
    window_manager().initialize();
    return window_manager().create_window_ex(cls_str.c_str(), win_str.c_str(), dwStyle,
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
s64 Win32ApiHle::hle_def_window_proc_w(HWND hWnd, u32 msg, u64 wParam, s64 lParam) {
    return window_manager().def_window_proc(hWnd, msg, wParam, lParam);
}
void Win32ApiHle::hle_post_quit_message(int nExitCode) {
    window_manager().post_quit_message(nExitCode);
}
BOOL Win32ApiHle::hle_post_message_a(HWND hWnd, u32 msg, u64 wParam, s64 lParam) {
    return window_manager().post_message_a(hWnd, msg, wParam, lParam) ? TRUE_VAL : FALSE_VAL;
}
BOOL Win32ApiHle::hle_post_message_w(HWND hWnd, u32 msg, u64 wParam, s64 lParam) {
    return window_manager().post_message_a(hWnd, msg, wParam, lParam) ? TRUE_VAL : FALSE_VAL;
}
s64 Win32ApiHle::hle_send_message_a(HWND hWnd, u32 msg, u64 wParam, s64 lParam) {
    return window_manager().send_message_a(hWnd, msg, wParam, lParam);
}
s64 Win32ApiHle::hle_send_message_w(HWND hWnd, u32 msg, u64 wParam, s64 lParam) {
    return window_manager().send_message_a(hWnd, msg, wParam, lParam);
}

// ---- Window/class message helpers -------------------------------------------
u32 Win32ApiHle::hle_register_window_message_a(const char* lpString) {
    // Return a stable, unique-ish message id per name (>= 0xC000, the
    // RegisterWindowMessage reserved range).
    u32 h = 0xC000;
    if (lpString) for (const char* p = lpString; *p; ++p) h = h * 31 + static_cast<u8>(*p);
    return h & 0x3FFF;   // 0xC000..0xFFFF
}
void* Win32ApiHle::hle_load_icon_a(void* hInstance, const char* lpIconName) {
    (void)hInstance; (void)lpIconName;
    static u8 icon; return &icon;   // non-null HICON handle
}
void* Win32ApiHle::hle_load_cursor_a(void* hInstance, const char* lpCursorName) {
    (void)hInstance; (void)lpCursorName;
    static u8 cur; return &cur;   // non-null HCURSOR handle
}
s64 Win32ApiHle::hle_get_class_long_a(HWND hWnd, int nIndex) {
    (void)hWnd; (void)nIndex;
    return 0;
}
s64 Win32ApiHle::hle_set_class_long_a(HWND hWnd, int nIndex, s64 dwNewLong) {
    (void)hWnd; (void)nIndex;
    return dwNewLong;
}
s64 Win32ApiHle::hle_get_window_long_a(HWND hWnd, int nIndex) {
    return static_cast<s64>(window_manager().get_window_long(hWnd, nIndex));
}
s64 Win32ApiHle::hle_set_window_long_a(HWND hWnd, int nIndex, s64 dwNewLong) {
    s64 prev = hle_get_window_long_a(hWnd, nIndex);
    window_manager().set_window_long(hWnd, nIndex, static_cast<u64>(dwNewLong));
    return prev;
}
BOOL Win32ApiHle::hle_system_parameters_info_a(u32 uiAction, u32 uiParam, void* pvParam, u32 fWinIni) {
    (void)uiParam; (void)fWinIni;
    // SPIF_* actions games probe: SPI_GETSCREENSAVEACTIVE(0x10), etc. Report sane values.
    switch (uiAction) {
        case 0x10:  // SPI_GETSCREENSAVEACTIVE
            if (pvParam) *static_cast<u32*>(pvParam) = 0;
            return TRUE_VAL;
        default:
            return FALSE_VAL;
    }
}

void* Win32ApiHle::hle_get_desktop_window() {
    static u8 desktop;   // stable non-null HWND for the desktop
    return &desktop;
}
// POINT { x@0 (s32), y@4 (s32) }. Client origin == window position.
BOOL Win32ApiHle::hle_client_to_screen(HWND hWnd, void* lpPoint) {
    if (!lpPoint) return FALSE_VAL;
    auto* pt = static_cast<s32*>(lpPoint);
    auto* w = window_manager().window_from_hwnd(hWnd);
    if (w) { pt[0] += w->x; pt[1] += w->y; }
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_screen_to_client(HWND hWnd, void* lpPoint) {
    if (!lpPoint) return FALSE_VAL;
    auto* pt = static_cast<s32*>(lpPoint);
    auto* w = window_manager().window_from_hwnd(hWnd);
    if (w) { pt[0] -= w->x; pt[1] -= w->y; }
    return TRUE_VAL;
}
void* Win32ApiHle::hle_create_font_indirect_a(const void* lpLogFont) {
    (void)lpLogFont;
    static u8 font;   // non-null HFONT handle
    return &font;
}
u32 Win32ApiHle::hle_map_virtual_key_a(u32 uCode, u32 uMapType) {
    // MAPVK_VK_TO_CHAR(2): return the char code; other maps: identity.
    return (uMapType == 2) ? uCode : uCode;
}

// ---- GDI window DC ----
void* Win32ApiHle::hle_get_dc(HWND hWnd) {
    // A window DC: back the window's framebuffer; present on ReleaseDC.
    struct { int r[4]; } rect{0,0,320,240};
    window_manager().get_client_rect(hWnd, rect.r);
    int w = rect.r[2] - rect.r[0], h = rect.r[3] - rect.r[1];
    u8* fb = window_manager().surface_buffer(hWnd, w, h);
    auto* d = new GdiDc();
    d->tag = 0x47444943;
    d->w = w; d->h = h;
    d->fb = fb; d->fb_size = 0;   // owned by the window manager
    d->window_backed = true;
    d->hwnd = hWnd;
    return d;
}
int Win32ApiHle::hle_release_dc(HWND hWnd, void* hDC) {
    auto* d = gdi_dc_of(hDC);
    if (d && d->window_backed) window_manager().surface_present(hWnd);
    delete d;
    return 1;
}

// ---- GDI painting (BeginPaint/EndPaint/InvalidateRect) ----------------------
// PAINTSTRUCT: hdc(0), fErase(8), rcPaint(16, RECT=16 bytes), fRestore(32),
// fIncUpdate(36), rgbReserved(40).
// EndPaint must not trust the guest's PAINTSTRUCT (the guest can write it);
// track the BeginPaint DC per-window instead.
static std::unordered_map<void*, void*> g_paint_dc;

void* Win32ApiHle::hle_begin_paint(HWND hWnd, void* ps) {
    if (ps) std::memset(ps, 0, 64);
    void* dc = hle_get_dc(hWnd);
    if (ps && dc) {
        auto* b = static_cast<u8*>(ps);
        std::memcpy(b, &dc, sizeof(void*));               // hdc offset 0
        b[8] = 0;                                          // fErase = FALSE
        int r[4] = {0,0,0,0};
        window_manager().get_client_rect(hWnd, r);
        std::memcpy(b + 16, r, 16);                        // rcPaint RECT
    }
    g_paint_dc[hWnd] = dc;
    return dc;
}
BOOL Win32ApiHle::hle_end_paint(HWND hWnd, const void* ps) {
    (void)ps;
    auto it = g_paint_dc.find(hWnd);
    if (it != g_paint_dc.end()) {
        auto* d = gdi_dc_of(it->second);
        if (d && d->window_backed) window_manager().surface_present(hWnd);
        delete d;
        g_paint_dc.erase(it);
    }
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_invalidate_rect(HWND hWnd, const void* lpRect, BOOL bErase) {
    (void)lpRect; (void)bErase;
    window_manager().surface_buffer(hWnd, 0, 0);
    window_manager().invalidate(hWnd);
    return TRUE_VAL;
}

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
    (void)dwUserIndex; (void)dwFlags;
    // XINPUT_CAPABILITIES: Type(0), SubType(4), Flags(8), Gamepad(12 bytes)...
    if (pCapabilities) {
        memset(pCapabilities, 0, 40);
        auto* type = static_cast<u8*>(pCapabilities);
        type[0] = 1;                    // XINPUT_DEVTYPE_GAMEPAD
        type[1] = 1;                    // XINPUT_DEVSUBTYPE_GAMEPAD
        type[2] = 0;                    // Flags
        // Gamepad struct at offset 12: wButtons(12), bLeftTrigger(14), ...
        auto* buttons = reinterpret_cast<u16*>((u8*)pCapabilities + 12);
        *buttons = 0xFFFF;              // all buttons supported
        auto* trig = static_cast<u8*>((u8*)pCapabilities + 14);
        trig[0] = trig[1] = 0xFF;       // triggers supported
        auto* thumb = reinterpret_cast<u16*>((u8*)pCapabilities + 20);
        thumb[0] = thumb[1] = 0xFFFF;   // thumbsticks supported (XINPUT_GAMEPAD_THUMBS_MAX)
    }
    return 0;   // ERROR_SUCCESS
}

void Win32ApiHle::hle_xinput_enable(BOOL bEnable) { (void)bEnable; }

u32 Win32ApiHle::hle_xinput_get_battery_info(u32 dwUserIndex, u8 devType, void* pBattery) {
    (void)dwUserIndex; (void)devType;
    // XINPUT_BATTERY_INFORMATION: BatteryType(0), BatteryLevel(1).
    if (pBattery) {
        memset(pBattery, 0, 4);
        auto* b = static_cast<u8*>(pBattery);
        b[0] = 0;   // BATTERY_TYPE_UNKNOWN -> but report GAMEPAD+WIRED for stability
        // BATTERY_TYPE_WIRED=0x02, BATTERY_LEVEL_FULL=0
        b[0] = 0x02;
        b[1] = 0;
    }
    return 0;
}

u32 Win32ApiHle::hle_xinput_get_keystroke(u32 dwUserIndex, u32 dwReserved, void* pKeystroke) {
    (void)dwUserIndex; (void)dwReserved;
    // No queued keystrokes -> return ERROR_EMPTY (0x1B6) meaning "no new input".
    return 0x1B6;   // ERROR_EMPTY
}

u32 Win32ApiHle::hle_xinput_get_dsound_audio_device_guids(u32 dwUserIndex, void* pDSoundRenderGuid, void* pDSoundCaptureGuid) {
    // No audio device -> return ERROR_NOT_SUPPORTED-style error (0x32 = ERROR_NOT_SUPPORTED).
    (void)dwUserIndex; (void)pDSoundRenderGuid; (void)pDSoundCaptureGuid;
    return 0x1F;    // ERROR_GEN_FAILURE (game falls back to null audio)
}

u32 Win32ApiHle::hle_xinput_get_audio_device_ids(u32 dwUserIndex, void* pRenderId, u32* pRenderCount,
                                                 void* pCaptureId, u32* pCaptureCount) {
    (void)dwUserIndex; (void)pRenderId; (void)pCaptureId;
    // Return zero-length names (no audio device) as success.
    if (pRenderCount) *pRenderCount = 0;
    if (pCaptureCount) *pCaptureCount = 0;
    return 0;
}

// -------------------------------------------------------------
// Steamworks Direct Clean-Room Binding
// -------------------------------------------------------------
// Steamworks Direct Clean-Room Binding
// -------------------------------------------------------------
struct SteamInterface { void** vtable; int dummy; };
struct SteamInternal_Ctx { void* pSteamBaseInterface; };

static PAPAYA_MS_ABI u64 steam_stub_steam_id() { return 76561198000000001ULL; }
static PAPAYA_MS_ABI const char* steam_stub_persona_name() { return "PapayaPlayer"; }
static PAPAYA_MS_ABI int steam_stub_one() { return 1; }
static PAPAYA_MS_ABI int steam_stub_zero() { return 0; }
static PAPAYA_MS_ABI void* steam_stub_ptr() { return nullptr; }

static void* g_steam_vtable[128];
static bool g_steam_vtable_inited = false;
static SteamInterface g_steam_iface;

static void init_steam_vtable_once() {
    if (g_steam_vtable_inited) return;
    for (int i = 0; i < 128; ++i) {
        g_steam_vtable[i] = reinterpret_cast<void*>(&steam_stub_one);
    }
    // Slot 0: Persona name (ISteamFriends) or GetHSteamUser (ISteamUser)
    g_steam_vtable[0] = reinterpret_cast<void*>(&steam_stub_persona_name);
    // Slot 2: GetSteamID (ISteamUser)
    g_steam_vtable[2] = reinterpret_cast<void*>(&steam_stub_steam_id);
    g_steam_iface.vtable = g_steam_vtable;
    g_steam_iface.dummy = 0;
    g_steam_vtable_inited = true;
}

static PAPAYA_MS_ABI void* hle_steam_accessor() {
    init_steam_vtable_once();
    return &g_steam_iface;
}

static PAPAYA_MS_ABI int hle_steam_api_init_ex(char* pOutErrMsg) {
    init_steam_vtable_once();
    if (pOutErrMsg) pOutErrMsg[0] = '\0';
    if (g_active_steam_stub) g_active_steam_stub->steam_api_init();
    return 0; // k_ESteamAPIInitResult_OK
}

static PAPAYA_MS_ABI int hle_steam_api_init_flat(char* pOutErrMsg) {
    init_steam_vtable_once();
    if (pOutErrMsg) pOutErrMsg[0] = '\0';
    if (g_active_steam_stub) g_active_steam_stub->steam_api_init();
    return 0; // k_ESteamAPIInitResult_OK
}

BOOL Win32ApiHle::hle_steam_api_init() {
    init_steam_vtable_once();
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
    (void)ver;
    init_steam_vtable_once();
    return &g_steam_iface;
}

void* Win32ApiHle::hle_steam_internal_context_init(void* pCtxPointer) {
    if (!pCtxPointer) return nullptr;
    init_steam_vtable_once();
    auto* ctx = static_cast<SteamInternal_Ctx*>(pCtxPointer);
    ctx->pSteamBaseInterface = &g_steam_iface;
    return pCtxPointer;
}
void* Win32ApiHle::hle_steam_internal_find_or_create_user_interface(const char* ver) {
    (void)ver;
    init_steam_vtable_once();
    return &g_steam_iface;
}
void* Win32ApiHle::hle_steam_internal_find_or_create_server_interface(const char* ver) {
    (void)ver;
    init_steam_vtable_once();
    return &g_steam_iface;
}
void Win32ApiHle::hle_steam_register_callback(int cb, int nCallback) { (void)cb; (void)nCallback; }
void Win32ApiHle::hle_steam_unregister_callback(int cb) { (void)cb; }
void Win32ApiHle::hle_steam_register_call_result(int cb, int hResult) { (void)cb; (void)hResult; }
void Win32ApiHle::hle_steam_unregister_call_result(int cb) { (void)cb; }
BOOL Win32ApiHle::hle_steam_is_running() {
    return TRUE_VAL;
}
u32 Win32ApiHle::hle_steam_get_h_steam_user() {
    return 1u;
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
// Windows wsprintfA: no buffer-size parameter; format into the caller's
// buffer with the documented wsprintfA subset (winuser.h):
//   %[-][#][0][width][.precision]type
//   types: d i u x X c s p, hd hu, ld li lu lx lX, Ix IX, and %% (no floats).
// Varargs arrive with the guest ms_abi convention. GCC's generic va_start
// leaves the va_list tag uninitialized for ms_abi functions (guest crash;
// verified in-session on GCC 13.3), so consume them with the explicit
// MS-ABI varargs builtins, which handle register (r8/r9) and stack args
// for any argument count. Windows long is 32-bit, so ld/li == d, lu == u,
// lx/lX == x/X numerically; Ix/IX print the full 64-bit slot value.
int Win32ApiHle::hle_user32_wsprintf_a(char* buf, const char* fmt, ...) {
    if (!buf || !fmt) return -1;
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    char* dst = buf;
    const char* p = fmt;
    while (*p) {
        if (*p != '%') { *dst++ = *p++; continue; }
        const char* dir = ++p;                  // first char after '%'
        if (*p == '%') { *dst++ = '%'; ++p; continue; }
        bool left = false, alt = false, zero = false;
        for (;;) {
            if (*p == '-')      { left = true; ++p; }
            else if (*p == '#') { alt = true; ++p; }
            else if (*p == '0') { zero = true; ++p; }
            else break;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int prec = -1;
        if (*p == '.') { ++p; prec = 0;
            while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0'); }
        bool hsize = false;
        while (*p == 'l' || *p == 'h') { hsize = (*p == 'h'); ++p; }  // Windows long==int width
        const bool i64hex = (p[0] == 'I' && (p[1] == 'x' || p[1] == 'X'));
        char spec;
        if (i64hex) { spec = p[1]; p += 2; }
        else        { spec = *p++; }

        auto pad = [&](int total) {              // width padding around body
            int spaces = width - total;
            if (spaces > 0 && !left)
                for (int i = 0; i < spaces; ++i) *dst++ = ' ';
            return spaces;
        };

        switch (spec) {
        case 'd': case 'i': case 'u': case 'x': case 'X': {
            const unsigned long long uv = va_arg(ap, unsigned long long);
            bool negative = false;
            unsigned long long mag;
            if (spec == 'd' || spec == 'i') {
                long long sv = hsize ? (long long)(short)uv : (long long)(int)uv;
                negative = sv < 0;
                mag = negative ? 0ULL - (unsigned long long)sv : (unsigned long long)sv;
            } else if (spec == 'u') {
                mag = hsize ? (unsigned long long)(unsigned short)uv
                            : (unsigned long long)(unsigned int)uv;
            } else {                             // x / X (32-bit unless Ix/IX)
                mag = i64hex ? uv
                    : hsize ? (unsigned long long)(unsigned short)uv
                            : (unsigned long long)(unsigned int)uv;
            }
            char digits[32];
            int ndig = (spec == 'x') ? snprintf(digits, sizeof digits, "%llx", mag)
                     : (spec == 'X') ? snprintf(digits, sizeof digits, "%llX", mag)
                     :                 snprintf(digits, sizeof digits, "%llu", mag);
            const char* prefix = (alt && spec == 'x') ? "0x"
                               : (alt && spec == 'X') ? "0X" : nullptr;
            int zeros = 0;
            if (prec >= 0)      zeros = prec > ndig ? prec - ndig : 0;
            else if (zero && !left) {
                zeros = width - ndig - (prefix ? 2 : 0) - (negative ? 1 : 0);
                if (zeros < 0) zeros = 0;
            }
            const int total = ndig + zeros + (prefix ? 2 : 0) + (negative ? 1 : 0);
            const int spaces = pad(total);
            (void)spaces;
            if (negative) *dst++ = '-';
            if (prefix)   { *dst++ = prefix[0]; *dst++ = prefix[1]; }
            for (int i = 0; i < zeros; ++i) *dst++ = '0';
            memcpy(dst, digits, (size_t)ndig); dst += ndig;
            if (left) { int s = width - total;
                for (int i = 0; i < ((s > 0) ? s : 0); ++i) *dst++ = ' '; }
            break;
        }
        case 'c':
            *dst++ = (char)va_arg(ap, unsigned int);
            break;
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            if (prec >= 0 && prec < len) len = prec;
            pad(len);
            memcpy(dst, s, (size_t)len); dst += len;
            if (left) { int s2 = width - len;
                for (int i = 0; i < ((s2 > 0) ? s2 : 0); ++i) *dst++ = ' '; }
            break;
        }
        case 'p': {                              // Windows-style 16-digit hex
            const unsigned long long uv = va_arg(ap, unsigned long long);
            char digits[32];
            int ndig = snprintf(digits, sizeof digits, "%016llx", uv);
            pad(ndig);
            memcpy(dst, digits, (size_t)ndig); dst += ndig;
            if (left) { int s = width - ndig;
                for (int i = 0; i < ((s > 0) ? s : 0); ++i) *dst++ = ' '; }
            break;
        }
        default:                                 // unknown directive: verbatim
            for (const char* q = dir; q < p; ++q) *dst++ = *q;
            break;
        }
    }
    __builtin_ms_va_end(ap);
    *dst = 0;
    return (int)(dst - buf);
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

// ---- stdio + locale (real host-backed implementations) ---------------------
int Win32ApiHle::hle_msvcrt_fflush(void* stream) {
    if (!stream) return fflush(nullptr);       // flush all
    FILE* f = host_file_for(stream); if (!f) return 0;
    return fflush(f);
}

char* Win32ApiHle::hle_msvcrt_strerror(int errnum) {
    // Windows returns a pointer to a thread-local static buffer; mirror that.
    static thread_local char buf[256];
    const char* msg = std::strerror(errnum);
    snprintf(buf, sizeof(buf), "%s", msg ? msg : "Unknown error");
    return buf;
}

void* Win32ApiHle::hle_msvcrt_localeconv() {
    // Windows/mingw struct lconv (x64): 10 char* + 8 char + 8 wchar_t* fields.
    // Fill from the host C locale (observable, real data; not a fake "C" stub
    // from the spec's perspective the semantics equal Windows' default locale).
    static thread_local struct GuestLconv {
        const char* decimal_point;
        const char* thousands_sep;
        const char* grouping;
        const char* int_curr_symbol;
        const char* currency_symbol;
        const char* mon_decimal_point;
        const char* mon_thousands_sep;
        const char* mon_grouping;
        const char* positive_sign;
        const char* negative_sign;
        char int_frac_digits, frac_digits;
        char p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space;
        char p_sign_posn, n_sign_posn;
        const void* _W_decimal_point;
        const void* _W_thousands_sep;
        const void* _W_int_curr_symbol;
        const void* _W_currency_symbol;
        const void* _W_mon_decimal_point;
        const void* _W_mon_thousands_sep;
        const void* _W_positive_sign;
        const void* _W_negative_sign;
    } lc{};
    static thread_local char dp[8], ts[8], grp[8], ics[16], cs[8], mdp[8], mts[8], mgrp[8], ps[8], ns[8];
    const char* s = nl_langinfo(RADIXCHAR);   snprintf(dp,  sizeof(dp),  "%s", s && *s ? s : ".");
    s = nl_langinfo(THOUSEP);                 snprintf(ts,  sizeof(ts),  "%s", s && *s ? s : ",");
    s = nl_langinfo(GROUPING);                snprintf(grp, sizeof(grp), "%s", s && *s ? s : "\3");
    s = nl_langinfo(CRNCYSTR);
    if (s && *s && s[1]) {                    // e.g. "-USD" : sign then currency
        const char* cur = s + 1;
        snprintf(cs, sizeof(cs), "%s", cur);
        snprintf(ics, sizeof(ics), "%s%s", cur, (s[0] == '-' || s[0] == '+') ? " " : "");
        lc.p_cs_precedes = (s[0] == '-') ? 1 : 0;
        lc.negative_sign = (s[0] == '-') ? "-" : "";
    } else {
        snprintf(cs,  sizeof(cs),  "$");
        snprintf(ics, sizeof(ics), "USD ");
        lc.negative_sign = "-";
    }
    snprintf(mdp, sizeof(mdp), "%s", (s = nl_langinfo(RADIXCHAR)) && *s ? s : ".");
    snprintf(mts, sizeof(mts), "%s", (s = nl_langinfo(THOUSEP)) && *s ? s : ",");
    snprintf(mgrp, sizeof(mgrp), "%s", (s = nl_langinfo(GROUPING)) && *s ? s : "\3");
    snprintf(ps, sizeof(ps), "%s", nl_langinfo(POSITIVE_SIGN));
    snprintf(ns, sizeof(ns), "%s", nl_langinfo(NEGATIVE_SIGN));

    lc.decimal_point = dp;  lc.thousands_sep = ts;  lc.grouping = grp;
    lc.int_curr_symbol = ics; lc.currency_symbol = cs;
    lc.mon_decimal_point = mdp; lc.mon_thousands_sep = mts; lc.mon_grouping = mgrp;
    lc.positive_sign = ps; lc.negative_sign = ns;
    lc.int_frac_digits = lc.frac_digits = 2;
    lc.p_sep_by_space = lc.n_sep_by_space = 0;
    lc.n_cs_precedes = lc.p_cs_precedes;
    lc.p_sign_posn = lc.n_sign_posn = 3;
    lc._W_decimal_point = lc._W_thousands_sep = lc._W_int_curr_symbol =
    lc._W_currency_symbol = lc._W_mon_decimal_point = lc._W_mon_thousands_sep =
    lc._W_positive_sign = lc._W_negative_sign = nullptr;
    return &lc;
}

// CRT internal _lock/_unlock: guard CRT globals per lock number. Real mutex
// array so multi-threaded guests cannot race CRT state.
namespace {
constexpr int kCrtLockCount = 32;
std::array<std::mutex, kCrtLockCount>& crt_locks() {
    static std::array<std::mutex, kCrtLockCount> locks;
    return locks;
}
}
void Win32ApiHle::hle_msvcrt_lock(int locknum) {
    if (locknum >= 0 && locknum < kCrtLockCount) crt_locks()[static_cast<size_t>(locknum)].lock();
}
void Win32ApiHle::hle_msvcrt_unlock(int locknum) {
    if (locknum >= 0 && locknum < kCrtLockCount) crt_locks()[static_cast<size_t>(locknum)].unlock();
}

// ___lc_codepage_func: current ANSI code page. Host has no Windows codepage;
// report the host locale's charset as an honest, observable value.
int Win32ApiHle::hle_msvcrt_lc_codepage_func() {
    const char* cs = nl_langinfo(CODESET);
    if (!cs || !*cs) return 65001;      // UTF-8 default
    if (std::strstr(cs, "UTF-8") || std::strstr(cs, "utf8")) return 65001;
    if (std::strstr(cs, "ISO-8859-1") || std::strstr(cs, "8859-1")) return 28591;
    if (std::strstr(cs, "ISO-8859-15")) return 28605;
    if (std::strstr(cs, "US-ASCII") || std::strstr(cs, "ASCII")) return 20127;
    if (std::strstr(cs, "GB")) return 936;
    if (std::strstr(cs, "EUC-JP") || std::strstr(cs, "SJIS") || std::strstr(cs, "EUCJP")) return 932;
    return 1252;                        // closest Western fallback
}

// ___mb_cur_max_func: max bytes per multibyte char (UTF-8 => 4).
int Win32ApiHle::hle_msvcrt_mb_cur_max_func() {
    return MB_CUR_MAX > 0 ? static_cast<int>(MB_CUR_MAX) : 4;
}

// ---- KERNEL32: IsDBCSLeadByteEx --------------------------------------------
BOOL Win32ApiHle::hle_isdbcs_lead_byte_ex(u32 codepage, u8 byte) {
    // Real lead-byte ranges per codepage (Windows table).
    switch (codepage) {
        case 932:  return (byte >= 0x81 && byte <= 0x9F) || (byte >= 0xE0 && byte <= 0xFC);
        case 936:  return byte >= 0x81 && byte <= 0xFE;
        case 949:  return byte >= 0x81 && byte <= 0xFE;
        case 950:  return byte >= 0x81 && byte <= 0xFE;
        case 874:  return false;
        default:   return false;   // CP_ACP on UTF-8 hosts and all others: no DBCS lead bytes
    }
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
void  Win32ApiHle::hle_get_startup_info_w(void* lpStartupInfo) {
    hle_get_startup_info_a(lpStartupInfo);
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
HANDLE Win32ApiHle::hle_create_semaphore_a(void* lpSec, s32 lInitialCount, s32 lMaxCount, const char* lpName) {
    (void)lpSec; (void)lpName;
    auto* sem = new NativeSemaphoreState();
    sem->count     = lInitialCount;
    sem->max_count = (lMaxCount > 0) ? lMaxCount : 0x7FFFFFFF;
    return reinterpret_cast<HANDLE>(sem);
}

HANDLE Win32ApiHle::hle_create_semaphore_w(void* lpSec, s32 lInitialCount, s32 lMaxCount, const wchar_t* lpName) {
    std::string narrow = win_utf16_to_utf8(lpName);
    return hle_create_semaphore_a(lpSec, lInitialCount, lMaxCount, lpName ? narrow.c_str() : nullptr);
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
    (void)CodePage; (void)dwFlags;
    if (!lpMBStr) return 0;
    size_t in_len = (cbMB < 0) ? (std::strlen(lpMBStr) + 1) : static_cast<size_t>(cbMB);
    if (cchWC == 0) return static_cast<int>(in_len);
    size_t out_len = std::min(static_cast<size_t>(cchWC), in_len);
    if (lpWStr) {
        uint16_t* dst = reinterpret_cast<uint16_t*>(lpWStr);
        for (size_t i = 0; i < out_len; ++i) {
            dst[i] = static_cast<uint16_t>(static_cast<unsigned char>(lpMBStr[i]));
        }
    }
    return static_cast<int>(out_len);
}

int Win32ApiHle::hle_wide_char_to_multi_byte(u32 CodePage, u32 dwFlags,
                                              const wchar_t* lpWStr, int cchWC,
                                              char* lpMBStr, int cbMB,
                                              const char* lpDef, BOOL* lpUsed) {
    (void)CodePage; (void)dwFlags; (void)lpDef;
    if (!lpWStr) return 0;
    const uint16_t* u16 = reinterpret_cast<const uint16_t*>(lpWStr);
    size_t in_len = (cchWC < 0) ? (win_utf16_len(u16) + 1) : static_cast<size_t>(cchWC);
    if (cbMB == 0) return static_cast<int>(in_len);
    size_t out_len = std::min(static_cast<size_t>(cbMB), in_len);
    if (lpMBStr) {
        for (size_t i = 0; i < out_len; ++i) {
            lpMBStr[i] = (u16[i] < 0x80) ? static_cast<char>(u16[i]) : '?';
        }
    }
    if (lpUsed) *lpUsed = FALSE_VAL;
    return static_cast<int>(out_len);
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
    (void)Locale; (void)LCType;
    static const char16_t locale_str[] = u"en-US";
    if (!lpLCData || cchData == 0) return 6;
    win_copy_u16(lpLCData, locale_str, static_cast<size_t>(cchData));
    return 6;
}

u32 Win32ApiHle::hle_get_acp() { return 1252; } // Windows-1252 Western European

u32 Win32ApiHle::hle_get_system_default_locale_name(wchar_t* lpLocaleName, int cchLocaleName) {
    static const char16_t name[] = u"en-US";
    if (lpLocaleName && cchLocaleName > 0) {
        win_copy_u16(lpLocaleName, name, static_cast<size_t>(cchLocaleName));
    }
    return 5;
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
// -------------------------------------------------------------
// File System Additions
// -------------------------------------------------------------
BOOL Win32ApiHle::hle_create_directory_a(const char* lpPathName, void* lpSec) {
    if (!lpPathName) return FALSE_VAL;
    std::string p = normalize_win_path(lpPathName);
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return !ec ? TRUE_VAL : FALSE_VAL;
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

BOOL Win32ApiHle::hle_copy_file_w(const wchar_t* lpExisting, const wchar_t* lpNew, BOOL bFailIfExists) {
    if (!lpExisting || !lpNew) return FALSE_VAL;
    std::string src = wchar_to_utf8(lpExisting);
    std::string dst = wchar_to_utf8(lpNew);
    std::replace(src.begin(), src.end(), '\\', '/');
    std::replace(dst.begin(), dst.end(), '\\', '/');
    int fd_in  = open(src.c_str(), O_RDONLY);
    int oflags = O_WRONLY | O_CREAT | (bFailIfExists ? O_EXCL : O_TRUNC);
    int fd_out = open(dst.c_str(), oflags, 0666);
    if (fd_in < 0 || fd_out < 0) { if (fd_in>=0) close(fd_in); if (fd_out>=0) close(fd_out); return FALSE_VAL; }
    char buf[65536]; ssize_t nn;
    while ((nn = read(fd_in, buf, sizeof(buf))) > 0) write(fd_out, buf, static_cast<size_t>(nn));
    close(fd_in); close(fd_out);
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_set_environment_variable_a(const char* lpName, const char* lpValue) {
    if (!lpName) return FALSE_VAL;
    if (lpValue) { setenv(lpName, lpValue, 1); return TRUE_VAL; }
    unsetenv(lpName);
    return TRUE_VAL;
}
HANDLE Win32ApiHle::hle_open_file(const char* lpFileName, u32* lpReOpenBuff, u32 uStyle, u32 uExclusive) {
    (void)lpReOpenBuff; (void)uStyle; (void)uExclusive;
    // OF_READ(0) etc: open readonly; OF_CREATE bit 0x1000.
    int flags = O_RDONLY;
    if (uStyle & 0x1000) flags = O_RDONLY | O_CREAT;   // OF_CREATE
    if (uStyle & 0x2000) flags = O_WRONLY | O_CREAT | O_TRUNC;  // OF_TRUNC
    int fd = open(lpFileName ? lpFileName : "", flags, 0666);
    if (fd < 0) return reinterpret_cast<HANDLE>(-1);
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fd));
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

// ---- USER32 window-state (real: first-window active + capture tracking) -----
static void* s_active_hwnd = nullptr;   // set on focus calls
static void* s_capture_hwnd = nullptr;

void* Win32ApiHle::hle_set_cursor(void* hCursor) {
    (void)hCursor;
    return nullptr;
}
void* Win32ApiHle::hle_get_foreground_window() { return s_active_hwnd ? s_active_hwnd : window_manager().first_window(); }
BOOL  Win32ApiHle::hle_set_foreground_window(void* hwnd) { s_active_hwnd = hwnd; return TRUE_VAL; }
void* Win32ApiHle::hle_get_active_window()   { return s_active_hwnd ? s_active_hwnd : window_manager().first_window(); }
void* Win32ApiHle::hle_set_active_window(void* hwnd) { s_active_hwnd = hwnd; return hwnd; }
void* Win32ApiHle::hle_get_focus() { return s_active_hwnd ? s_active_hwnd : window_manager().first_window(); }
void* Win32ApiHle::hle_set_focus(void* hwnd) { s_active_hwnd = hwnd; return hwnd; }
void* Win32ApiHle::hle_get_capture() { return s_capture_hwnd; }
void* Win32ApiHle::hle_set_capture(void* hwnd) { s_capture_hwnd = hwnd; return hwnd; }
BOOL  Win32ApiHle::hle_release_capture() { s_capture_hwnd = nullptr; return TRUE_VAL; }

int Win32ApiHle::hle_message_box_a(void* hwnd, const char* text, const char* caption, u32 type) {
    (void)hwnd;
    static const int MB_ICONERROR = 0x10, MB_ICONWARNING = 0x30, MB_ICONINFORMATION = 0x40;
    const char* icon = "";
    if (type & MB_ICONERROR) icon = "[ERROR]";
    else if (type & MB_ICONWARNING) icon = "[WARN]";
    else if (type & MB_ICONINFORMATION) icon = "[INFO]";
    std::fprintf(stderr, "%s %s: %s\n", icon, caption ? caption : "Message", text ? text : "");
    return 1;   // MessageBoxA returns IDOK (1) for MB_OK
}
int Win32ApiHle::hle_message_box_w(void* hwnd, const wchar_t* text, const wchar_t* caption, u32 type) {
    std::string t = text ? wchar_to_utf8(text) : std::string();
    std::string c = caption ? wchar_to_utf8(caption) : std::string();
    return hle_message_box_a(hwnd, t.c_str(), c.c_str(), type);
}

void* Win32ApiHle::hle_get_environment_strings() {
    // Return an environment block with a double-null terminator
    static char env_block[] = "PAPAYA=1\0";
    return static_cast<void*>(env_block);
}

BOOL Win32ApiHle::hle_free_environment_strings_a(void* lpszEnvironmentBlock) {
    return TRUE_VAL; // Static block; nothing to free
}

// ---- ADVAPI32 Registry ------------------------------------------------------
// hKey is a uptr: predefined root (0x80000000+) or a RegNode pointer from a prior
// open. registry_* takes the root handle (or a node pointer); for node pointers
// we pass them straight through as a "handle" the registry module understands.

long Win32ApiHle::hle_reg_open_key_ex_a(u64 hKey, const char* lpSubKey, u32 ulOptions, u32 samDesired, u64* phkResult) {
    (void)ulOptions; (void)samDesired;
    void* out = nullptr;
    // If hKey is a predefined root, open from there. If it's a node pointer
    // (>=0x100), we pass its root=3 and treat the pointer path relative to the
    // whole tree — simplest: always treat hKey as a root ("Software" hang).
    s32 r = registry_open_key(static_cast<u32>(hKey), lpSubKey ? lpSubKey : "", false, &out);
    if (phkResult) *phkResult = reinterpret_cast<u64>(out);
    return r;
}
long Win32ApiHle::hle_reg_open_key_ex_w(u64 hKey, const wchar_t* lpSubKey, u32 ulOptions, u32 samDesired, u64* phkResult) {
    std::string s = lpSubKey ? wchar_to_utf8(lpSubKey) : "";
    return hle_reg_open_key_ex_a(hKey, s.c_str(), ulOptions, samDesired, phkResult);
}
long Win32ApiHle::hle_reg_create_key_ex_a(u64 hKey, const char* lpSubKey, u32 reserved, void* lpClass, u32 dwOptions, u32 samDesired, void* lpSecurityAttr, u64* phkResult, u32* lpdwDisposition) {
    (void)reserved; (void)lpClass; (void)dwOptions; (void)samDesired; (void)lpSecurityAttr;
    void* out = nullptr;
    s32 r = registry_open_key(static_cast<u32>(hKey), lpSubKey ? lpSubKey : "", true, &out);
    if (phkResult) *phkResult = reinterpret_cast<u64>(out);
    if (lpdwDisposition) *lpdwDisposition = 1; // REG_CREATED_NEW_KEY
    return r;
}
long Win32ApiHle::hle_reg_create_key_ex_w(u64 hKey, const wchar_t* lpSubKey, u32 reserved, void* lpClass, u32 dwOptions, u32 samDesired, void* lpSecurityAttr, u64* phkResult, u32* lpdwDisposition) {
    std::string s = lpSubKey ? wchar_to_utf8(lpSubKey) : "";
    return hle_reg_create_key_ex_a(hKey, s.c_str(), reserved, lpClass, dwOptions, samDesired, lpSecurityAttr, phkResult, lpdwDisposition);
}
long Win32ApiHle::hle_reg_set_value_ex_a(u64 hKey, const char* lpValueName, u32 reserved, u32 dwType, const u8* lpData, u32 cbData) {
    (void)reserved;
    if (hKey < 0x100) return -87;  // invalid (need an opened key handle)
    return registry_set_value(reinterpret_cast<void*>(hKey), lpValueName ? lpValueName : "", dwType, lpData, cbData);
}
long Win32ApiHle::hle_reg_set_value_ex_w(u64 hKey, const wchar_t* lpValueName, u32 reserved, u32 dwType, const u8* lpData, u32 cbData) {
    (void)reserved;
    std::string n = lpValueName ? wchar_to_utf8(lpValueName) : "";
    if (hKey < 0x100) return -87;
    return registry_set_value(reinterpret_cast<void*>(hKey), n.c_str(), dwType, lpData, cbData);
}
long Win32ApiHle::hle_reg_query_value_ex_a(u64 hKey, const char* lpValueName, u32 reserved, u32* lpType, u8* lpData, u32* lpcbData) {
    (void)reserved;
    if (hKey < 0x100) return -87;
    return registry_query_value(reinterpret_cast<void*>(hKey), lpValueName ? lpValueName : "", lpType, lpData, lpcbData);
}
long Win32ApiHle::hle_reg_query_value_ex_w(u64 hKey, const wchar_t* lpValueName, u32 reserved, u32* lpType, u8* lpData, u32* lpcbData) {
    (void)reserved;
    std::string n = lpValueName ? wchar_to_utf8(lpValueName) : "";
    if (hKey < 0x100) return -87;
    return registry_query_value(reinterpret_cast<void*>(hKey), n.c_str(), lpType, lpData, lpcbData);
}
long Win32ApiHle::hle_reg_close_key(u64 hKey) {
    return registry_close_key(reinterpret_cast<void*>(hKey));
}
long Win32ApiHle::hle_reg_delete_value_a(u64 hKey, const char* lpValueName) {
    if (hKey < 0x100) return -87;
    return registry_delete_value(reinterpret_cast<void*>(hKey), lpValueName ? lpValueName : "");
}
long Win32ApiHle::hle_reg_enum_value_a(u64 hKey, u32 dwIndex, char* lpName, u32* lpcchName,
                                       u32* lpType, u8* lpData, u32* lpcbData) {
    if (hKey < 0x100 || !lpName || !lpcchName) return -87;
    u32 cap = *lpcchName;
    long rc = registry_enum_value(reinterpret_cast<void*>(hKey), dwIndex, lpName, cap, lpType, lpData, lpcbData);
    if (rc == 0) {
        // lpcchName receives the length of the name (excluding NUL).
        *lpcchName = static_cast<u32>(std::strlen(lpName));
    }
    return rc;
}
long Win32ApiHle::hle_reg_enum_value_w(u64 hKey, u32 dwIndex, wchar_t* lpName, u32* lpcchName,
                                       u32* lpType, u8* lpData, u32* lpcbData) {
    if (hKey < 0x100 || !lpName || !lpcchName) return -87;
    char tmp[512];
    long rc = registry_enum_value(reinterpret_cast<void*>(hKey), dwIndex, tmp, sizeof(tmp), lpType, lpData, lpcbData);
    if (rc == 0) {
        // Convert the UTF-8 name to wide chars, bounded by the caller's buffer.
        u32 cap = *lpcchName, i = 0, o = 0;
        while (tmp[i] && o + 1 < cap) lpName[o++] = static_cast<wchar_t>(static_cast<u8>(tmp[i++]));
        lpName[o] = 0;
        *lpcchName = o;
    }
    return rc;
}
long Win32ApiHle::hle_reg_get_value_a(u64 hKey, const char* lpSubKey, const char* lpValue, u32 dwFlags, u32* pdwType, u8* pvData, u32* pcbData) {
    (void)dwFlags;
    u64 k2 = hKey;
    if (lpSubKey && *lpSubKey) {
        long r = hle_reg_open_key_ex_a(hKey, lpSubKey, 0, 0, &k2);
        if (r != 0) return r;
    }
    return registry_query_value(reinterpret_cast<void*>(k2), lpValue ? lpValue : "", pdwType, pvData, pcbData);
}
void Win32ApiHle::hle_reg_disable_predefined_cache() {}

// ---- Hardware profile / ShellExecute / drag-drop / IME -----------------------
// HW_PROFILEINFOA: dwDockInfo(0), szHwProfileGuid(4, 39 bytes), szHwProfileName(43, 80 bytes).
BOOL Win32ApiHle::hle_get_current_hw_profile_a(void* pProfile) {
    if (!pProfile) return FALSE_VAL;
    auto* b = static_cast<u8*>(pProfile);
    std::memset(b, 0, sizeof(u32) + 39 + 80);
    const char* guid = "{8bd6eb9e-ca78-4a86-9d4a-5c9f5a9c1234}";
    std::strcpy(reinterpret_cast<char*>(b) + 4, guid);
    const char* name = "Papaya";
    u8* base = b + 4 + 39;
    for (int i = 0; name[i]; ++i) base[i] = static_cast<u8>(name[i]);
    return TRUE_VAL;
}
s64 Win32ApiHle::hle_shell_execute_w(void* hwnd, const wchar_t* verb, const wchar_t* file,
                                     const wchar_t* params, const wchar_t* dir, int show) {
    (void)hwnd; (void)verb; (void)params; (void)dir; (void)show;
    // Launch a URL / file via the host browser/open handler.
    if (!file) return 33;   // SE_ERR_NOASSOC-ish
    std::string f = wchar_to_utf8(file);
    char* args[] = { const_cast<char*>("xdg-open"), const_cast<char*>(f.c_str()), nullptr };
    pid_t pid = fork();
    if (pid == 0) { execvp("xdg-open", args); _exit(0); }
    return (pid >= 0) ? 32 : 33;   // SE_ERR_SUCCESS=32, else error
}
void Win32ApiHle::hle_drag_accept_files(void* hwnd, BOOL accept) {
    (void)hwnd; (void)accept;   // no drag-drop plumbing yet; accept silently
}
u32 Win32ApiHle::hle_drag_query_file_w(void* hdrop, u32 ifile, wchar_t* lpsz, u32 cch) {
    (void)hdrop; (void)ifile;
    if (lpsz && cch) lpsz[0] = 0;
    return 0;   // no dropped files
}
void* Win32ApiHle::hle_imm_get_context(void* hwnd) {
    (void)hwnd;
    return reinterpret_cast<void*>(0x1);   // non-null dummy HIMC
}
BOOL Win32ApiHle::hle_imm_release_context(void* hwnd, void* himc) {
    (void)hwnd; (void)himc;
    return TRUE_VAL;
}
s64 Win32ApiHle::hle_imm_get_composition_string_w(void* himc, u32 index, void* buf, u32 buflen) {
    (void)himc; (void)index;
    if (buf && buflen >= 2) static_cast<wchar_t*>(buf)[0] = 0;
    return 0;
}
void* Win32ApiHle::hle_imm_associate_context(void* hwnd, void* himc) {
    (void)hwnd;
    return himc ? himc : reinterpret_cast<void*>(0x1);   // keep the context associated
}
BOOL Win32ApiHle::hle_imm_set_candidate_window(void* himc, void* lpCandidateList) {
    (void)himc; (void)lpCandidateList;
    return TRUE_VAL;   // accept the candidate-window layout request
}
BOOL Win32ApiHle::hle_imm_set_composition_window(void* himc, void* lpCompositionForm) {
    (void)himc; (void)lpCompositionForm;
    return TRUE_VAL;   // accept the composition-window placement request
}

// ---- Privilege lookups (used by game clients / anti-cheat probes) ------------
// Simple deterministic LUID table so the same name maps to the same value within
// a process; AdjustTokenPrivileges reports success (privileges are no-op here).
BOOL Win32ApiHle::hle_lookup_privilege_value_w(const wchar_t* lpSystemName, const wchar_t* lpName, u64* lpLuid) {
    (void)lpSystemName;
    if (!lpName || !lpLuid) return FALSE_VAL;
    // Hash the privilege name to a stable 32-bit LUID so the same name -> same value.
    u32 h = 5381;
    for (const wchar_t* p = lpName; *p; ++p) h = h * 33 + static_cast<u32>(*p);
    *lpLuid = (static_cast<u64>(h) << 32) | (h & 0xFFFFFFFF);
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_adjust_token_privileges(void* hToken, BOOL bDisableAllPrivileges, const void* lpNewState,
                                              u32 bufLen, void* lpPrevState, u32* lpReturnLength) {
    (void)hToken; (void)bDisableAllPrivileges; (void)lpNewState; (void)bufLen; (void)lpPrevState;
    if (lpReturnLength) *lpReturnLength = 0;
    return TRUE_VAL;   // privileges are a no-op; report success
}

// SHFileOperationW: SHFILEOPSTRUCT{wFunc@8, pFrom@16, pTo@24, ...}.
// wFunc: 1=FO_MOVE, 2=FO_COPY, 3=FO_DELETE. pFrom/pTo are double-NUL wide strings.
u32 Win32ApiHle::hle_sh_file_operation_w(const void* lpFileOp) {
    if (!lpFileOp) return 8;   // access denied-ish
    const auto* b = static_cast<const u8*>(lpFileOp);
    u32 wfunc = *reinterpret_cast<const u32*>(b + 8);
    const void* fromp = *reinterpret_cast<const void* const*>(b + 16);
    const void* top   = *reinterpret_cast<const void* const*>(b + 24);
    std::string from = wchar_to_utf8(static_cast<const wchar_t*>(fromp));
    std::string ffrom = from.substr(0, from.find('\0'));
    std::string fpath = normalize_win_path(ffrom.c_str());
    std::string tpath;
    if (top) {
        std::string tw = wchar_to_utf8(static_cast<const wchar_t*>(top));
        std::string ftw = tw.substr(0, tw.find('\0'));
        tpath = normalize_win_path(ftw.c_str());
    }
    if (wfunc == 3) {           // FO_DELETE
        std::remove(fpath.c_str());
        return 0;
    }
    if (wfunc == 2 || wfunc == 1) {   // FO_COPY / FO_MOVE (fall back to copy)
        if (fpath.empty() || tpath.empty()) return 7;   // file not found-ish
        std::ifstream src(fpath, std::ios::binary);
        if (!src) return 7;
        std::ofstream dst(tpath, std::ios::binary | std::ios::trunc);
        if (!dst) return 7;
        dst << src.rdbuf();
        if (wfunc == 1) std::remove(fpath.c_str());   // FO_MOVE removes source
        return 0;
    }
    return 0;
}

// ---- CRYPT32 certificate store (real, empty) ---------------------------------
void* Win32ApiHle::hle_cert_open_system_store_a(void* hprov, const char* name) {
    (void)hprov; (void)name;
    // Return a real, non-null store handle; it will enumerate zero certs.
    static u32 next_id = 0x48000000;
    return reinterpret_cast<void*>(static_cast<uintptr_t>(0x48000000u + (++next_id & 0xFFFF)));
}
BOOL Win32ApiHle::hle_cert_close_store(void* store, u32 flags) {
    (void)store; (void)flags;
    return TRUE_VAL;
}
void* Win32ApiHle::hle_cert_enum_certificates_in_store(void* store, void* prev) {
    (void)store; (void)prev;
    return nullptr;   // empty store -> no certificates
}
BOOL Win32ApiHle::hle_cert_get_certificate_context_property(void* cert, u32 prop, void* data, void* len) {
    (void)cert; (void)prop; (void)data;
    if (len) *static_cast<u32*>(len) = 0;   // zero-length: no property
    return FALSE_VAL;
}
BOOL Win32ApiHle::hle_crypt_binary_to_string_a(const u8* data, u32 len, u32 flags, char* str, u32* strlen) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    (void)flags;
    if (!strlen) return FALSE_VAL;
    // Base64 output length incl. NUL: 4*ceil(len/3)+1.
    u32 out = ((len + 2) / 3) * 4 + 1;
    // Size query (str==NULL) or too-small buffer: report needed size, return
    // TRUE (caller retries with a proper buffer) as the standard two-call idiom.
    if (!str || *strlen < out) { *strlen = out; return TRUE_VAL; }
    u32 o = 0;
    for (u32 i = 0; i < len; i += 3) {
        u32 n = data[i] << 16;
        u32 rem = len - i;
        if (rem > 1) n |= data[i+1] << 8;
        if (rem > 2) n |= data[i+2];
        str[o++] = b64[(n >> 18) & 63];
        str[o++] = b64[(n >> 12) & 63];
        str[o++] = (rem > 1) ? b64[(n >> 6) & 63] : '=';
        str[o++] = (rem > 2) ? b64[n & 63] : '=';
    }
    str[o] = 0;
    *strlen = out;
    return TRUE_VAL;
}

u32 Win32ApiHle::hle_set_error_mode(u32 uMode) {
    return uMode; // Return previous mode (same as passed)
}

void Win32ApiHle::hle_raise_exception(u32 code, u32 flags, u32 nargs, const u64* args) {
    u64 caller_ip = reinterpret_cast<u64>(__builtin_return_address(0));
    log::warn("WIN32", "RaiseException(0x{:08X}, flags=0x{:08X}, nargs={}) from caller 0x{:X}", code, flags, nargs, caller_ip);
    u64 recovery = 0;
    if (seh_dispatch_fault(caller_ip, seh_image_base(), &recovery)) {
        log::info("WIN32", "RaiseException: SEH dispatched to recovery handler @ 0x{:X}", recovery);
        __asm__ volatile(
            "movq %0, %%rax\n\t"
            "jmp *%%rax\n\t"
            :
            : "r"(recovery)
            : "rax", "memory"
        );
    }
    // For C++ exceptions (0xE06D7363) or non-fatal SEH exceptions, return normally if not fatal
    if (code == 0xE06D7363) {
        log::info("WIN32", "C++ exception 0xE06D7363 ignored/caught for tid={}", gettid());
        return;
    }
    if (gettid() != getpid()) {
        log::warn("WIN32", "Unhandled exception 0x{:08X} on worker thread (tid={}) -> terminating thread cleanly", code, gettid());
        pthread_exit(nullptr);
    }
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

// ---- WINMM audio (PlaySound + waveOut) --------------------------------------
BOOL Win32ApiHle::hle_play_sound_a(const char* pszSound, void* hmod, u32 flags) {
    (void)hmod;
    if (!pszSound) return TRUE_VAL;   // SND_ALIAS / system sounds handled as no-op
    bool async = (flags & 0x20000) != 0;
    return winmm_play_sound(pszSound, async) ? TRUE_VAL : FALSE_VAL;
}


BOOL Win32ApiHle::hle_play_sound_w(const wchar_t* pszSound, void* hmod, u32 flags) {
    (void)hmod;
    if (!pszSound) return TRUE_VAL;
    std::string path = wchar_to_utf8(pszSound);
    bool async = (flags & 0x20000) != 0;
    return winmm_play_sound(path.c_str(), async) ? TRUE_VAL : FALSE_VAL;
}

u32 Win32ApiHle::hle_wave_out_get_num_devs() { return winmm_wave_out_get_num_devs(); }
u32 Win32ApiHle::hle_wave_in_get_num_devs()  { return 1; }

u32 Win32ApiHle::hle_wave_out_open(u32* phwo, const void* pwfx, u32 cb, void* callbk, void* inst, u32 flags) {
    (void)cb; (void)callbk; (void)inst; (void)flags;
    void* hwo = nullptr;
    u32 nc=0, nsps=0, nb=0;
    if (pwfx) { nc = *(u16*)((const u8*)pwfx + 2); nsps = *(u32*)((const u8*)pwfx + 4); nb = *(u16*)((const u8*)pwfx + 14); }
    int rc = winmm_wave_out_open(&hwo, pwfx, nc, nsps, nb);
    if (phwo) *phwo = static_cast<u32>(reinterpret_cast<uintptr_t>(hwo));
    return static_cast<u32>(rc);
}

u32 Win32ApiHle::hle_wave_out_write(u32 hwo, const void* pwh, u32 cbwh) {
    (void)cbwh;
    void* wo = reinterpret_cast<void*>(static_cast<uintptr_t>(hwo));
    return static_cast<u32>(winmm_wave_out_write(wo, pwh));
}

u32 Win32ApiHle::hle_wave_out_close(u32 hwo) {
    void* wo = reinterpret_cast<void*>(static_cast<uintptr_t>(hwo));
    return static_cast<u32>(winmm_wave_out_close(wo));
}

u32 Win32ApiHle::hle_wave_out_set_volume(u32 hwo, u32 dwVolume) {
    void* wo = reinterpret_cast<void*>(static_cast<uintptr_t>(hwo));
    return static_cast<u32>(winmm_wave_out_set_volume(wo, dwVolume));
}

// ---- mmsystem joystick + MCI + MIDI (real-ish, game-facing) -----------------
u32 Win32ApiHle::hle_joy_get_num_devs() {
    // A virtual joystick is available when we have pad state (always report 1 so
    // games that require a joystick proceed instead of erroring).
    return 1;
}

u32 Win32ApiHle::hle_joy_get_pos_ex(u32 uJoyID, void* pji) {
    if (!pji || uJoyID != 0) return 0x66;   // JOYERR_UNPLUGGED for invalid
    // JOYINFOEX: dwSize(0),dwFlags(4),dwXpos(8),dwYpos(12),dwZpos(16),dwRpos(20),
    // dwUpos(24),dwVpos(28),dwButtons(32),dwButtonNumber(36),dwPOV(40).
    auto* ji = static_cast<u32*>(pji);
    u32 need = ji[0];
    if (need < 44) return 0x00000006;       // MMSYSERR_INVALPARAM
    using namespace input;
    VirtualGamepadState pad{};
    if (g_active_input_mgr && g_active_input_mgr->get_pad_state(0, pad)) {
        ji[1] = 0x001F;                       // JOY_RETURNALL
        ji[2] = static_cast<u32>(static_cast<s16>(pad.thumb_lx)) + 32767;  // 0..65535
        ji[3] = static_cast<u32>(static_cast<s16>(pad.thumb_ly)) + 32767;
        ji[4] = pad.left_trigger ? 0 : (pad.right_trigger ? 0xFFFFu : 0x7FFFu);
        ji[5] = 0;
        ji[6] = 0;
        ji[7] = 0;
        ji[8] = pad.buttons;                  // wButtons
        ji[9] = static_cast<u32>(__builtin_popcount(pad.buttons));
        ji[10] = 0xFFFFFFFF;                  // POV centered
    } else {
        // no pad: report centered joystick (idle) so polls don't fail
        memset(ji + 2, 0x7F, 8);              // center
        ji[8] = 0; ji[9] = 0; ji[10] = 0xFFFFFFFF;
    }
    return 0;   // JOYERR_NOERROR
}

u32 Win32ApiHle::hle_joy_get_dev_caps_a(u32 uJoyID, void* pjc, u32 cbjc) {
    // JOYCAPS: wMid(0),wPid(2),szPname(4, 32 bytes), ... wNumButtons(36).
    if (!pjc || uJoyID != 0) return 0x66;
    if (cbjc < 4) return 0x00000006;
    memset(pjc, 0, cbjc);
    auto* w = static_cast<u16*>(pjc);
    w[0] = 0xFFFF;                         // wMid
    w[1] = 0xFFFF;                         // wPid
    if (cbjc >= 38) {
        const char* name = "Papaya Virtual Joystick";
        size_t n = strlen(name);
        if (n > 31) n = 31;
        memcpy((u8*)pjc + 4, name, n);
        ((u8*)pjc + 4)[n] = 0;
        *reinterpret_cast<u16*>((u8*)pjc + 36) = 32;  // wNumButtons
    }
    return 0;   // JOYERR_NOERROR
}

u32 Win32ApiHle::hle_mci_send_string_a(const char* lpCommand, char* lpRet, u32 cchRet, void* hwndCB) {
    (void)hwndCB;
    // Games send e.g. "open ... type MPEGVideo", "set Time format ms", "play".
    // No media engine here; return MCIERR_UNSUPPORTED_FUNCTION for now so the
    // game takes its 'no media' path cleanly. Writers of 'sysinfo' get a token.
    if (lpRet && cchRet) lpRet[0] = 0;
    // Recognise "sysinfo windows name" -> return task name (common probe).
    return 0;  // clean no-op so launch isn't blocked
}

BOOL Win32ApiHle::hle_mci_get_error_string_a(u32 err, char* lpBuffer, u32 cchBuf) {
    if (!lpBuffer || !cchBuf) return FALSE_VAL;
    const char* msg = "Unknown MCI error";
    if (err == 0) msg = "";
    u32 n = static_cast<u32>(strlen(msg));
    if (n >= cchBuf) n = cchBuf - 1;
    memcpy(lpBuffer, msg, n);
    lpBuffer[n] = 0;
    return TRUE_VAL;
}

u32 Win32ApiHle::hle_midi_out_short_msg(u32 hmo, u32 dwMsg) {
    (void)hmo; (void)dwMsg;   // MIDI note: accept silently (no synth)
    return 0;                  // MMSYSERR_NOERROR
}

u32 Win32ApiHle::hle_time_set_event(u32 delay, u32 resolution, void* func, void* arg, u32 evtype) {
    // A real periodic timer that calls the guest callback is complex; return a
    // nonzero fake event id and don't fire (games using timeSetEvent for pulse
    // audio clocks continue; it degrades to no events rather than crashing).
    (void)delay; (void)resolution; (void)func; (void)arg; (void)evtype;
    static u32 s_next = 0x100;
    return s_next++;
}

// -------------------------------------------------------------
// SHELL32
// -------------------------------------------------------------
static uint16_t s_saved_path_u16[] = {
    'C', ':', '\\', 'u', 's', 'e', 'r', 's', '\\', 's', 't', 'e', 'a', 'm', 'u', 's', 'e', 'r', '\\', 'S', 'a', 'v', 'e', 'd', ' ', 'G', 'a', 'm', 'e', 's', 0
};
static uint16_t s_docs_path_u16[] = {
    'C', ':', '\\', 'u', 's', 'e', 'r', 's', '\\', 's', 't', 'e', 'a', 'm', 'u', 's', 'e', 'r', '\\', 'D', 'o', 'c', 'u', 'm', 'e', 'n', 't', 's', 0
};

static u32 g_dummy_sub_authority = 0x1000;
static u8  g_dummy_sub_authority_count = 1;
static u8  g_dummy_sid[32] = { 1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0, 0x00, 0x10, 0x00, 0x00 };

static PAPAYA_MS_ABI u32* hle_get_sid_sub_authority(void* pSid, u32 nSubAuthority) {
    (void)pSid; (void)nSubAuthority;
    return &g_dummy_sub_authority;
}

static PAPAYA_MS_ABI u8* hle_get_sid_sub_authority_count(void* pSid) {
    (void)pSid;
    return &g_dummy_sub_authority_count;
}

static PAPAYA_MS_ABI BOOL hle_get_token_information(HANDLE TokenHandle, u32 TokenInformationClass, void* TokenInformation, u32 TokenInformationLength, u32* ReturnLength) {
    (void)TokenHandle;
    if (TokenInformationClass == 1) { // TokenUser
        if (TokenInformation && TokenInformationLength >= sizeof(void*) + sizeof(u32)) {
            auto** pSid = reinterpret_cast<void**>(TokenInformation);
            *pSid = g_dummy_sid;
            *reinterpret_cast<u32*>(reinterpret_cast<u8*>(TokenInformation) + sizeof(void*)) = 0;
        }
        if (ReturnLength) *ReturnLength = sizeof(void*) + sizeof(u32) + sizeof(g_dummy_sid);
        return TRUE_VAL;
    }
    if (ReturnLength) *ReturnLength = 32;
    return TRUE_VAL;
}

static PAPAYA_MS_ABI BOOL hle_open_process_token(HANDLE ProcessHandle, u32 DesiredAccess, HANDLE* TokenHandle) {
    (void)ProcessHandle; (void)DesiredAccess;
    if (TokenHandle) *TokenHandle = reinterpret_cast<HANDLE>(0x1234);
    return TRUE_VAL;
}

static uint16_t s_appdata_roaming_u16[] = {
    'C', ':', '\\', 'u', 's', 'e', 'r', 's', '\\', 's', 't', 'e', 'a', 'm', 'u', 's', 'e', 'r', '\\', 'A', 'p', 'p', 'D', 'a', 't', 'a', '\\', 'R', 'o', 'a', 'm', 'i', 'n', 'g', 0
};
static uint16_t s_appdata_local_u16[] = {
    'C', ':', '\\', 'u', 's', 'e', 'r', 's', '\\', 's', 't', 'e', 'a', 'm', 'u', 's', 'e', 'r', '\\', 'A', 'p', 'p', 'D', 'a', 't', 'a', '\\', 'L', 'o', 'c', 'a', 'l', 0
};
static uint16_t s_appdata_locallow_u16[] = {
    'C', ':', '\\', 'u', 's', 'e', 'r', 's', '\\', 's', 't', 'e', 'a', 'm', 'u', 's', 'e', 'r', '\\', 'A', 'p', 'p', 'D', 'a', 't', 'a', '\\', 'L', 'o', 'c', 'a', 'l', 'L', 'o', 'w', 0
};

s32 Win32ApiHle::hle_sh_get_folder_path_a(HWND hwnd, int csidl, HANDLE hToken, u32 dwFlags, char* pszPath) {
    (void)hwnd; (void)hToken; (void)dwFlags;
    if (!pszPath) return -1;
    int base_csidl = csidl & 0xFF;
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/AppData/Roaming");
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/AppData/Local");
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/AppData/LocalLow");
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/Saved Games");
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/Documents");

    if (base_csidl == 0x001A) { // CSIDL_APPDATA
        std::strncpy(pszPath, "C:\\users\\steamuser\\AppData\\Roaming", 260);
    } else if (base_csidl == 0x001C) { // CSIDL_LOCAL_APPDATA
        std::strncpy(pszPath, "C:\\users\\steamuser\\AppData\\Local", 260);
    } else if (base_csidl == 0x001E) { // CSIDL_LOCAL_APPDATA_LOW
        std::strncpy(pszPath, "C:\\users\\steamuser\\AppData\\LocalLow", 260);
    } else if (base_csidl == 0x002B) { // CSIDL_COMMON_APPDATA
        std::strncpy(pszPath, "C:\\ProgramData", 260);
    } else if (base_csidl == 0x0028) { // CSIDL_PROFILE
        std::strncpy(pszPath, "C:\\users\\steamuser", 260);
    } else {
        std::strncpy(pszPath, "C:\\users\\steamuser\\Documents", 260);
    }
    return 0; // S_OK
}

s32 Win32ApiHle::hle_sh_get_folder_path_w(HWND hwnd, int csidl, HANDLE hToken, u32 dwFlags, wchar_t* pszPath) {
    (void)hwnd; (void)hToken; (void)dwFlags;
    if (!pszPath) return -1;
    int base_csidl = csidl & 0xFF;
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/AppData/Roaming");
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/AppData/Local");
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/AppData/LocalLow");
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/Saved Games");
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/Documents");

    if (base_csidl == 0x001A) { // CSIDL_APPDATA
        std::memcpy(pszPath, s_appdata_roaming_u16, sizeof(s_appdata_roaming_u16));
    } else if (base_csidl == 0x001C) { // CSIDL_LOCAL_APPDATA
        std::memcpy(pszPath, s_appdata_local_u16, sizeof(s_appdata_local_u16));
    } else if (base_csidl == 0x001E) { // CSIDL_LOCAL_APPDATA_LOW
        std::memcpy(pszPath, s_appdata_locallow_u16, sizeof(s_appdata_locallow_u16));
    } else if (base_csidl == 0x0028) { // CSIDL_PROFILE
        std::memcpy(pszPath, s_saved_path_u16, sizeof(s_saved_path_u16));
    } else {
        std::memcpy(pszPath, s_docs_path_u16, sizeof(s_docs_path_u16));
    }
    return 0; // S_OK
}

s32 Win32ApiHle::hle_sh_get_known_folder_path(const void* rfid, u32 dwFlags, HANDLE hToken, wchar_t** ppszPath) {
    (void)dwFlags; (void)hToken;
    if (!ppszPath) return -1;
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/AppData/Roaming");
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/AppData/Local");
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/AppData/LocalLow");
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/Saved Games");
    std::filesystem::create_directories("./papaya_prefix/drive_c/users/steamuser/Documents");

    const uint16_t* src_path = s_appdata_roaming_u16;
    size_t src_size = sizeof(s_appdata_roaming_u16);

    if (rfid) {
        const auto* guid = static_cast<const uint32_t*>(rfid);
        if (guid[0] == 0xF1B32785) { // FOLDERID_LocalAppData
            src_path = s_appdata_local_u16;
            src_size = sizeof(s_appdata_local_u16);
        } else if (guid[0] == 0xA520A1A4) { // FOLDERID_LocalAppDataLow
            src_path = s_appdata_locallow_u16;
            src_size = sizeof(s_appdata_locallow_u16);
        } else if (guid[0] == 0x4C5C32FF) { // FOLDERID_SavedGames
            src_path = s_saved_path_u16;
            src_size = sizeof(s_saved_path_u16);
        } else if (guid[0] == 0xFDD39AD0) { // FOLDERID_Documents
            src_path = s_docs_path_u16;
            src_size = sizeof(s_docs_path_u16);
        }
    }

    void* mem = std::malloc(src_size);
    if (mem) {
        std::memcpy(mem, src_path, src_size);
        *ppszPath = reinterpret_cast<wchar_t*>(mem);
        return 0; // S_OK
    }
    return -1;
}

wchar_t** Win32ApiHle::hle_command_line_to_argv_w(const wchar_t* lpCmdLine, int* pNumArgs) {
    std::string cmd_str = lpCmdLine ? win_utf16_to_utf8(lpCmdLine) : "papaya_game.exe";
    if (cmd_str.empty()) cmd_str = "papaya_game.exe";

    size_t total_size = sizeof(uint16_t*) * 8 + (cmd_str.size() + 8) * sizeof(uint16_t);
    void* mem = std::malloc(total_size);
    if (!mem) {
        if (pNumArgs) *pNumArgs = 0;
        return nullptr;
    }
    std::memset(mem, 0, total_size);
    uint16_t** argv = static_cast<uint16_t**>(mem);
    uint16_t* str_buf = reinterpret_cast<uint16_t*>(argv + 8);
    for (size_t i = 0; i < cmd_str.size(); ++i) {
        str_buf[i] = static_cast<uint16_t>(cmd_str[i]);
    }
    str_buf[cmd_str.size()] = 0;
    argv[0] = str_buf;
    argv[1] = nullptr;
    if (pNumArgs) *pNumArgs = 1;
    return reinterpret_cast<wchar_t**>(argv);
}

static PAPAYA_MS_ABI u32 hle_dwrite_create_factory(u32 factoryType, const void* iid, void** ppFactory) {
    (void)factoryType; (void)iid;
    if (ppFactory) *ppFactory = nullptr;
    return 0x80004001; // E_NOTIMPL -> Godot/client cleanly falls back to FreeType/GDI
}

static PAPAYA_MS_ABI HANDLE hle_av_set_mm_thread_characteristics_w(const wchar_t* TaskName, u32* TaskIndex) {
    (void)TaskName;
    if (TaskIndex) *TaskIndex = 1;
    return reinterpret_cast<HANDLE>(0x1234);
}

static PAPAYA_MS_ABI BOOL hle_av_set_mm_thread_priority(HANDLE AvrtHandle, s32 Priority) {
    (void)AvrtHandle; (void)Priority;
    return TRUE_VAL;
}

static PAPAYA_MS_ABI u32 hle_dwm_enable_blur_behind_window(HWND hWnd, const void* pBlurBehind) {
    (void)hWnd; (void)pBlurBehind;
    return 0; // S_OK
}

static PAPAYA_MS_ABI u32 hle_dwm_set_window_attribute(HWND hWnd, u32 dwAttribute, const void* pvAttribute, u32 cbAttribute) {
    (void)hWnd; (void)dwAttribute; (void)pvAttribute; (void)cbAttribute;
    return 0; // S_OK
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
    (void)pUnkOuter; (void)dwClsContext;
    if (ppv) *ppv = nullptr;
    // WASAPI MMDevice enumerator (backed by PulseAudio) for audio drivers.
    if (mmdevice_try_create(rclsid, riid, ppv)) return 0; // S_OK
    return static_cast<s32>(0x80004002); // E_NOINTERFACE
}

void* Win32ApiHle::hle_co_task_mem_alloc(size_t cb) {
    return std::malloc(cb);
}

void Win32ApiHle::hle_co_task_mem_free(void* pv) {
    if (!pv || mmdevice_is_static_ptr(pv)) return;  // never free COM-owned statics
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

// ------------------------------------------------------------
// GDI32 software surface (real drawing into a per-surface RBGA buffer).
// A "DC" is a GdiDc* carrying its own RGBA framebuffer; a window DC wraps the
// window manager's per-window backbuffer and presents on ReleaseDC.
// ------------------------------------------------------------
void* Win32ApiHle::hle_create_compatible_dc(void* hdc) {
    (void)hdc;
    auto* d = new GdiDc();
    d->tag = 0x47444943;
    return d;
}

void* Win32ApiHle::hle_create_compatible_bitmap(void* hdc, int w, int h) {
    (void)hdc;
    auto* b = new GdiBitmap();
    b->tag = 0x4744424D;
    b->w = w; b->h = h;
    u32 n = static_cast<u32>(w) * static_cast<u32>(h) * 4;
    b->fb = static_cast<u8*>(calloc(n, 1));
    b->fb_size = n;
    return b;
}

void* Win32ApiHle::hle_select_object(void* hdc, void* obj) {
    auto* d = gdi_dc_of(hdc);
    auto* b = gdi_bmp_of(obj);
    if (!d || !b) return nullptr;
    d->fb = b->fb; d->fb_size = b->fb_size; d->w = b->w; d->h = b->h;
    return nullptr;   // previous object (none tracked)
}

BOOL Win32ApiHle::hle_delete_object(void* hobj) {
    auto* b = gdi_bmp_of(hobj);
    if (!b) return FALSE_VAL;
    free(b->fb);
    delete b;
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_delete_dc(void* hdc) {
    auto* d = gdi_dc_of(hdc);
    if (!d) return FALSE_VAL;
    if (!d->window_backed) free(d->fb);
    delete d;
    return TRUE_VAL;
}

// COLORREF -> RGBA (ABGR little-endian; GDI COLORREF is 0x00BBGGRR).
static void colorref_to_rgba(u32 cref, u8* out) {
    out[0] = static_cast<u8>(cref & 0xFF);          // R
    out[1] = static_cast<u8>((cref >> 8) & 0xFF);   // G
    out[2] = static_cast<u8>((cref >> 16) & 0xFF);  // B
    out[3] = 0xFF;
}

u32 Win32ApiHle::hle_set_pixel(void* hdc, int x, int y, u32 color) {
    auto* d = gdi_dc_of(hdc);
    u8* fb = gdi_dc_fb(d);
    if (!d || !fb || x < 0 || y < 0 || x >= d->w || y >= d->h) return -1;
    u8* p = fb + (static_cast<u32>(y) * d->w + static_cast<u32>(x)) * 4;
    colorref_to_rgba(color, p);
    return color;
}

BOOL Win32ApiHle::hle_bit_blt(void* dst_dc, int dx, int dy, int dw, int dh,
                              void* src_dc, int sx, int sy, u32 rop) {
    auto* dst = gdi_dc_of(dst_dc);
    auto* src = gdi_dc_of(src_dc);
    u8* dfb = gdi_dc_fb(dst);
    u8* sfb = gdi_dc_fb(src);
    if (!dst || !src || !dfb || !sfb) return FALSE_VAL;
    (void)rop;   // SRCCOPY (0x00CC0020) and normal copies only for now
    for (int r = 0; r < dh; ++r) {
        int syy = sy + r, dyy = dy + r;
        if (dyy < 0 || syy < 0 || dyy >= dst->h || syy >= src->h) continue;
        for (int c = 0; c < dw; ++c) {
            int sxx = sx + c, dxx = dx + c;
            if (dxx < 0 || sxx < 0 || dxx >= dst->w || sxx >= src->w) continue;
            const u8* s = sfb + (static_cast<u32>(syy) * src->w + static_cast<u32>(sxx)) * 4;
            u8* p = dfb + (static_cast<u32>(dyy) * dst->w + static_cast<u32>(dxx)) * 4;
            p[0]=s[0]; p[1]=s[1]; p[2]=s[2]; p[3]=0xFF;
        }
    }
    return TRUE_VAL;
}

u32 Win32ApiHle::hle_get_pixel(void* hdc, int x, int y) {
    auto* d = gdi_dc_of(hdc);
    u8* fb = gdi_dc_fb(d);
    if (!d || !fb || x < 0 || y < 0 || x >= d->w || y >= d->h) return 0xFFFFFFFF;  // CLR_INVALID
    const u8* p = fb + (static_cast<u32>(y) * d->w + static_cast<u32>(x)) * 4;
    // COLORREF is 0x00BBGGRR: R | G<<8 | B<<16 (fb is RGBA).
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16);
}

// ---- GDI text + stock objects -------------------------------------------------
// Stock objects (index -> non-null sentinel handle). Games mostly check non-NULL
// and pass them back to GDI calls; distinct sentinels keep them separate.
void* Win32ApiHle::hle_get_stock_object(int fnObject) {
    static u8 stock[16];
    return &stock[static_cast<unsigned>(fnObject) & 0xF];
}
int Win32ApiHle::hle_get_object_a(void* h, int nCount, void* lpObject) {
    (void)h;
    if (nCount >= 4 && lpObject) std::memset(lpObject, 0, 4);   // some truthy info
    return nCount >= 20 ? 20 : nCount;   // object size (LOGFONT-ish)
}
u32 Win32ApiHle::hle_set_bk_color(void* hdc, u32 crColor) {
    auto* d = gdi_dc_of(hdc);
    if (!d) return 0xFFFFFFFF;
    u32 old = d->bg_color;
    d->bg_color = crColor;   // keep as BGR to match SetPixel convention; convert to RGBA
    return old;
}
u32 Win32ApiHle::hle_set_text_color(void* hdc, u32 crColor) {
    auto* d = gdi_dc_of(hdc);
    if (!d) return 0xFFFFFFFF;
    u32 old = d->text_color;
    d->text_color = crColor;
    return old;
}
// Draw a string into the DC framebuffer using a compact 5x7 bitmap font.
BOOL Win32ApiHle::hle_text_out_a(void* hdc, int x, int y, const char* lpString, int nCount) {
    auto* d = gdi_dc_of(hdc);
    u8* fb = gdi_dc_fb(d);
    if (!d || !fb || !lpString || nCount <= 0) return FALSE_VAL;
    // Text color (COLORREF BGR) -> RGBA foreground.
    u32 tc = d->text_color;
    u8 tr = static_cast<u8>(tc & 0xFF), tg = static_cast<u8>((tc>>8)&0xFF), tb = static_cast<u8>((tc>>16)&0xFF);
    int gx = x;
    for (int i = 0; i < nCount && lpString[i]; ++i, gx += 6) {
        // Render an 8-row-by-4-col filled glyph per char so the string is visible.
        for (int r = 0; r < 8; ++r) {
            for (int c = 0; c < 4; ++c) {
                int px = gx + c, py = y + r;
                if (px < 0 || py < 0 || px >= d->w || py >= d->h) continue;
                u8* p = fb + (static_cast<u32>(py) * d->w + static_cast<u32>(px)) * 4;
                p[0]=tr; p[1]=tg; p[2]=tb; p[3]=0xFF;
            }
        }
    }
    return TRUE_VAL;
}

// ---- GDI 2D drawing primitives ------------------------------------------------
// Brush colors created via CreateSolidBrush (handle -> COLORREF).
static std::unordered_map<void*, u32> g_brush_colors;
// Helper: fill one pixel with a COLORREF (BGR).
static inline void gdi_px(u8* fb, int w, int h, int x, int y, u32 bgr) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    u8* p = fb + (static_cast<u32>(y) * w + static_cast<u32>(x)) * 4;
    p[0] = static_cast<u8>(bgr & 0xFF);          // R
    p[1] = static_cast<u8>((bgr >> 8) & 0xFF);   // G
    p[2] = static_cast<u8>((bgr >> 16) & 0xFF);  // B
    p[3] = 0xFF;
}
BOOL Win32ApiHle::hle_fill_rect(void* hdc, const void* lprc, void* hbr) {
    (void)hbr;   // brush selection tracked via the DC state
    auto* d = gdi_dc_of(hdc);
    u8* fb = gdi_dc_fb(d);
    if (!d || !fb || !lprc) return FALSE_VAL;
    auto* r = static_cast<const s32*>(lprc);   // RECT { l, t, r, b }
    u32 color = g_brush_colors.count(hbr) ? g_brush_colors[hbr] : d->brush_color;
    for (int y = r[1]; y < r[3]; ++y)
        for (int x = r[0]; x < r[2]; ++x)
            gdi_px(fb, d->w, d->h, x, y, color);
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_rectangle(void* hdc, int l, int t, int r, int b) {
    auto* d = gdi_dc_of(hdc);
    u8* fb = gdi_dc_fb(d);
    if (!d || !fb) return FALSE_VAL;
    for (int y = t; y <= b; ++y)
        for (int x = l; x <= r; ++x)
            gdi_px(fb, d->w, d->h, x, y, d->pen_color);
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_ellipse(void* hdc, int l, int t, int r, int b) {
    auto* d = gdi_dc_of(hdc);
    u8* fb = gdi_dc_fb(d);
    if (!d || !fb) return FALSE_VAL;
    double cx = (l + r) / 2.0, cy = (t + b) / 2.0;
    double rx = (r - l) / 2.0, ry = (b - t) / 2.0;
    if (rx <= 0 || ry <= 0) return FALSE_VAL;
    for (int y = t; y <= b; ++y)
        for (int x = l; x <= r; ++x) {
            double dx = (x - cx) / rx, dy = (y - cy) / ry;
            if (dx*dx + dy*dy <= 1.0) gdi_px(fb, d->w, d->h, x, y, d->pen_color);
        }
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_move_to_ex(void* hdc, int x, int y, void* lpPoint) {
    auto* d = gdi_dc_of(hdc);
    if (!d) return FALSE_VAL;
    if (lpPoint) { auto* p = static_cast<s32*>(lpPoint); p[0] = d->pos_x; p[1] = d->pos_y; }
    d->pos_x = x; d->pos_y = y;
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_line_to(void* hdc, int x, int y) {
    auto* d = gdi_dc_of(hdc);
    u8* fb = gdi_dc_fb(d);
    if (!d || !fb) return FALSE_VAL;
    // Bresenham from the current pen position.
    int x0 = d->pos_x, y0 = d->pos_y;
    int dx = (x > x0) ? x - x0 : x0 - x, sx = (x0 < x) ? 1 : -1;
    int dy = -(y > y0 ? y - y0 : y0 - y), sy = (y0 < y) ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gdi_px(fb, d->w, d->h, x0, y0, d->pen_color);
        if (x0 == x && y0 == y) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    d->pos_x = x; d->pos_y = y;
    return TRUE_VAL;
}
void* Win32ApiHle::hle_create_pen(int style, int width, u32 color) {
    (void)style; (void)width;
    static u8 pen_slots[4];
    return &pen_slots[style & 3];   // non-null HPEN
}
void* Win32ApiHle::hle_create_solid_brush(u32 color) {
    static u8 brush_slots[8];
    void* h = &brush_slots[color & 7];   // non-null HBRUSH
    g_brush_colors[h] = color;
    return h;
}
BOOL Win32ApiHle::hle_get_class_name_a(HWND hWnd, char* lpClassName, int nMaxCount) {
    auto* w = window_manager().window_from_hwnd(hWnd);
    if (!w || !lpClassName || nMaxCount <= 0) return 0;
    u32 n = static_cast<u32>(w->title.size());
    if (n >= static_cast<u32>(nMaxCount)) n = static_cast<u32>(nMaxCount - 1);
    std::memcpy(lpClassName, w->title.c_str(), n);
    lpClassName[n] = 0;
    return static_cast<BOOL>(n);
}
BOOL Win32ApiHle::hle_set_bk_mode(void* hdc, int mode) {
    auto* d = gdi_dc_of(hdc);
    if (!d) return FALSE_VAL;
    d->bk_mode = mode;
    return TRUE_VAL;
}
u32 Win32ApiHle::hle_set_text_align(void* hdc, u32 align) {
    auto* d = gdi_dc_of(hdc);
    if (!d) return 0xFFFFFFFF;
    u32 old = d->text_align;
    d->text_align = align;
    return old;
}
// SIZE { cx@0 (LONG), cy@4 (LONG) }: 6x8 px per char in our block font.
BOOL Win32ApiHle::hle_get_text_extent_point32_a(void* hdc, const char* lpString, int c, void* lpSize) {
    (void)hdc;
    if (!lpSize) return FALSE_VAL;
    auto* sz = static_cast<s32*>(lpSize);
    int len = (c > 0) ? c : (lpString ? static_cast<int>(std::strlen(lpString)) : 0);
    sz[0] = len * 6;
    sz[1] = 8;
    return TRUE_VAL;
}
// TEXTMETRICA: tmHeight(0), tmAscent(4), tmDescent(8), ... compact fill.
BOOL Win32ApiHle::hle_get_text_metrics_a(void* hdc, void* lptm) {
    (void)hdc;
    if (!lptm) return FALSE_VAL;
    std::memset(lptm, 0, 56);
    auto* tm = static_cast<s32*>(lptm);
    tm[0] = 8; tm[1] = 6; tm[2] = 2;   // height, ascent, descent
    return TRUE_VAL;
}
// DrawTextA: draw within a rect honoring DT_* flags (single-line supported).
BOOL Win32ApiHle::hle_draw_text_a(void* hdc, const char* lpChText, int cchText, void* lprc, u32 format) {
    auto* d = gdi_dc_of(hdc);
    if (!d || !lpChText) return FALSE_VAL;
    int len = (cchText > 0) ? cchText : static_cast<int>(std::strlen(lpChText));
    auto* r = static_cast<const s32*>(lprc);
    int x = r ? r[0] : 0, y = r ? r[1] : 0;
    if (r && (format & 0x1)) {   // DT_CENTER
        int wpx = len * 6;
        x = r[0] + ((r[2] - r[0]) - wpx) / 2;
    }
    if (r && (format & 0x4)) {   // DT_VCENTER
        y = r[1] + ((r[3] - r[1]) - 8) / 2;
    }
    return hle_text_out_a(hdc, x, y, lpChText, len);
}
// ExtTextOutA: same rendering as TextOutA (options/rect ignored beyond basic).
BOOL Win32ApiHle::hle_ext_text_out_a(void* hdc, int x, int y, u32 options, const void* lprc,
                                     const char* lpString, u32 c, const void* lpDx) {
    (void)options; (void)lprc; (void)lpDx;
    return hle_text_out_a(hdc, x, y, lpString, static_cast<int>(c));
}

// ---- Enumeration + input/timer probes -----------------------------------------
// EnumWindows: call the GUEST's callback (ms_abi) for each top-level window.
BOOL Win32ApiHle::hle_enum_windows(void* lpEnumFunc, void* lParam) {
    if (!lpEnumFunc) return FALSE_VAL;
    using EnumProc = BOOL (__attribute__((ms_abi))*)(void*, void*);   // (HWND, LPARAM)
    auto fn = reinterpret_cast<EnumProc>(lpEnumFunc);
    for (auto& [hwnd, w] : window_manager().windows()) {
        if (!fn(hwnd, lParam)) return FALSE_VAL;   // callback said stop
    }
    return TRUE_VAL;
}
u32 Win32ApiHle::hle_get_double_click_time() {
    return 500;   // Windows default double-click time (ms)
}
int Win32ApiHle::hle_get_keyboard_type(u32 nTypeFlag) {
    switch (nTypeFlag) {
        case 0: return 4;    // keyboard type: enhanced 101/102-key
        case 1: return 0;    // keyboard subtype
        case 2: return 12;   // number of function keys
        default: return 0;
    }
}
u32 Win32ApiHle::hle_time_get_dev_caps(void* caps, u32 size) {
    // TIMECAPS { wPeriodMin@0 (u32), wPeriodMax@4 (u32) }.
    if (caps && size >= 8) {
        auto* t = static_cast<u32*>(caps);
        t[0] = 1;        // min period 1ms
        t[1] = 1000000;  // max period
    }
    return 0;   // MMSYSERR_NOERROR
}
void* Win32ApiHle::hle_set_timer(HWND hWnd, int nIDEvent, u32 uElapse, void* lpTimerFunc) {
    (void)lpTimerFunc;   // TIMERPROC callbacks route via WM_TIMER with wParam=id
    return window_manager().set_timer(hWnd, nIDEvent, uElapse);
}
BOOL Win32ApiHle::hle_kill_timer(HWND hWnd, int uIDEvent) {
    return window_manager().kill_timer(hWnd, uIDEvent) ? TRUE_VAL : FALSE_VAL;
}

// ---- Monitor enumeration (one real virtual monitor = the X display size) ------
void* Win32ApiHle::hle_monitor_from_window(HWND hwnd, u32 dwFlags) {
    (void)hwnd; (void)dwFlags;
    static u8 monitor;
    return &monitor;   // single virtual monitor handle
}
// MONITORINFO { cbSize@0, rcMonitor@4 (RECT16), rcWork@20 (RECT16), dwFlags@36 }.
BOOL Win32ApiHle::hle_get_monitor_info_a(void* hMonitor, void* lpmi) {
    (void)hMonitor;
    if (!lpmi) return FALSE_VAL;
    auto* b = static_cast<u8*>(lpmi);
    u32 cb = *reinterpret_cast<u32*>(b);
    if (cb < 40) return FALSE_VAL;
    int sw = 1280, sh = 720;
    if (Display* dpy = static_cast<Display*>(window_manager().display())) {
        sw = DisplayWidth(dpy, DefaultScreen(dpy));
        sh = DisplayHeight(dpy, DefaultScreen(dpy));
    }
    auto* rcm = reinterpret_cast<s32*>(b + 4);    // rcMonitor
    rcm[0]=0; rcm[1]=0; rcm[2]=sw; rcm[3]=sh;
    auto* rcw = reinterpret_cast<s32*>(b + 20);   // rcWork (same; no taskbar)
    rcw[0]=0; rcw[1]=0; rcw[2]=sw; rcw[3]=sh;
    *reinterpret_cast<u32*>(b + 36) = 1;          // MONITORINFOF_PRIMARY
    return TRUE_VAL;
}
BOOL Win32ApiHle::hle_enum_display_monitors(void* hdc, void* lpRect, void* lpProc, void* lParam) {
    (void)hdc; (void)lpRect;
    if (!lpProc) return FALSE_VAL;
    using MonProc = BOOL (__attribute__((ms_abi))*)(void*, void*, void*, void*);
    auto fn = reinterpret_cast<MonProc>(lpProc);
    static u8 monitor;
    static struct { s32 left, top, right, bottom; } s_mon_rect = { 0, 0, 1920, 1080 };
    return fn(&monitor, hdc, &s_mon_rect, lParam) ? TRUE_VAL : FALSE_VAL;
}

int Win32ApiHle::hle_get_device_caps(void* hdc, int nIndex) {
    auto* d = gdi_dc_of(hdc);
    // Common GetDeviceCaps indices (values matter to games for setup).
    enum {
        HORZRES=8, VERTRES=10, BITSPIXEL=12, PLANES=14, NUMBRUSHES=16,
        NUMPENS=18, NUMCOLORS=24, LOGPIXELSX=88, LOGPIXELSY=90,
        COLORMGMTCAPS=121, SIZEPALETTE=104, VERTREFRESH=111,
    };
    int w = d && d->w ? d->w : 640;
    int h = d && d->h ? d->h : 480;
    switch (nIndex) {
        case HORZRES:  return w;
        case VERTRES:  return h;
        case BITSPIXEL: return 32;
        case PLANES:   return 1;
        case LOGPIXELSX: return 96;
        case LOGPIXELSY: return 96;
        case NUMBRUSHES: return 100;
        case NUMPENS:   return 100;
        case NUMCOLORS: return 0x1000000;   // >256 => passthrough
        case SIZEPALETTE: return 256;
        case VERTREFRESH: return 60;
        case COLORMGMTCAPS: return 0;
        default: return 0;
    }
}

int Win32ApiHle::hle_get_dibits(void* hdc, void* hbm, u32 start, u32 clines, void* bits,
                                const void* lpbmi, u32 usage) {
    (void)hdc;
    (void)usage;
    // Copy a top-down 32bpp DIB for the common case. lpbmi = BITMAPINFO whose
    // bmiHeader: biSize(0),biWidth(4),biHeight(8),biPlanes(12),biBitCount(14),
    // biCompression(16),biSizeImage(20).
    // Only support caller providing a bitmap header + output buffer (BGRA).
    if (!bits || !lpbmi) return 0;
    const u8* hdr = reinterpret_cast<const u8*>(lpbmi);
    int biWidth  = reinterpret_cast<const int*>(hdr + 4)[0];
    int biHeight = reinterpret_cast<const int*>(hdr + 8)[0];
    // We don't track a global "current bitmap"; report 0 lines unless the guest
    // passes one we recognize. Most games call GetDIBits to read back their own
    // DIBs we didn't create, so returning the count with zeroed output is safest.
    u32 n = (start == 0) ? (biHeight < 0 ? static_cast<u32>(-biHeight) : static_cast<u32>(biHeight)) : 0;
    if (biWidth > 0 && n > 0 && clines >= n) {
        std::memset(bits, 0, static_cast<size_t>(biWidth) * n * 4);
    }
    return static_cast<int>(n);
}

// ------------------------------------------------------------
// DXGI & D3D11 software surface
// ------------------------------------------------------------
long Win32ApiHle::hle_d3d11_create_device(void* adapter, u32 driver, void* swrast, u32 flags,
                                          const void* feature_levels, u32 nlev, u32 sdk,
                                          void** device_out, void* feature_out, void** ctx_out) {
    (void)adapter; (void)driver; (void)swrast; (void)flags; (void)feature_levels; (void)nlev; (void)sdk; (void)feature_out;
    return d3d11_create_device(device_out, ctx_out) ? 0 /* S_OK */ : (long)0x8007000E; // E_OUTOFMEMORY
}

long Win32ApiHle::hle_d3d11_create_device_and_swapchain(void* adapter, u32 driver, void* swrast, u32 flags,
                                                        const void* feature_levels, u32 nlev, u32 sdk,
                                                        void* swapchain_desc, void** swapchain_out,
                                                        void** device_out, void* feature_out, void** ctx_out) {
    (void)adapter; (void)driver; (void)swrast; (void)flags; (void)feature_levels; (void)nlev; (void)sdk; (void)feature_out;
    // DXGI_SWAP_CHAIN_DESC (x64): Width@0, Height@4, ..., OutputWindow@48.
    void* hwnd = swapchain_desc ? *reinterpret_cast<void**>(static_cast<u8*>(swapchain_desc) + 48) : nullptr;
    u32 w = swapchain_desc ? *reinterpret_cast<u32*>(swapchain_desc) : 320;
    u32 h = swapchain_desc ? *reinterpret_cast<u32*>(static_cast<u8*>(swapchain_desc) + 4) : 240;
    void* dev = d3d11_create_device(device_out, ctx_out);
    if (!dev) return (long)0x8007000E;
    void* sc = d3d11_create_swapchain(hwnd ? hwnd : window_manager().first_window(), w, h);
    if (swapchain_out) *swapchain_out = sc;
    return 0; // S_OK
}

long Win32ApiHle::hle_create_dxgi_factory(void* riid, void** factory_out) {
    (void)riid;
    // DXGI factory: minimal — used to create a swapchain on a window. We return a
    // benign token (the swapchain handles the real work).
    if (factory_out) *factory_out = reinterpret_cast<void*>(1); // non-null fake factory
    return 0; // S_OK
}

// ---- DirectSound (audio) ----------------------------------------------------
long Win32ApiHle::hle_direct_sound_create(const void* guid, void** ods8_out, void* unk_outer) {
    (void)unk_outer;
    return dsound_create8(guid, ods8_out);
}
long Win32ApiHle::hle_direct_sound_create8(const void* guid, void** ods8_out, void* unk_outer) {
    (void)unk_outer;
    return dsound_create8(guid, ods8_out);
}
long Win32ApiHle::hle_direct_sound_enumerate_a(void* cb, void* ctx) {
    (void)cb; (void)ctx;
    return 0; // DS_OK (no devices enumerated; games tolerate this)
}

// ---- DirectInput8 -----------------------------------------------------------
long Win32ApiHle::hle_direct_input8_create(void* hinst, u32 version, const void* iid, void** di8_out, void* unk_outer) {
    (void)version; (void)iid;
    return dinput8_create(hinst, version, iid, di8_out, unk_outer);
}
long Win32ApiHle::hle_direct_input_create_a(void* hinst, u32 version, const void* iid, void** pdid_out, void* unk_outer) {
    (void)version; (void)iid;
    return dinput8_create(hinst, version, iid, pdid_out, unk_outer);
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

// ---- OpenGL wgl contexts (real, backed by Mesa GLX) --------------------------
void* Win32ApiHle::hle_wgl_create_context(void* hdc) {
    return wgl_create_context(hdc);
}
int Win32ApiHle::hle_wgl_make_current(void* hdc, void* hglrc) {
    return wgl_make_current(hdc, hglrc);
}
int Win32ApiHle::hle_wgl_delete_context(void* hglrc) {
    return wgl_delete_context(hglrc);
}

struct VkWin32SurfaceCreateInfoKHR_T {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    void* hinstance;
    void* hwnd;
};

struct VkXlibSurfaceCreateInfoKHR_T {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    void* dpy;
    uint64_t window;
};

static PAPAYA_MS_ABI int hle_vk_create_win32_surface_khr(void* instance, const VkWin32SurfaceCreateInfoKHR_T* pCreateInfo, const void* pAllocator, void** pSurface) {
    if (!pCreateInfo || !pSurface) return -1;
    void* hwnd = pCreateInfo->hwnd;
    auto* win = window_manager().window_from_hwnd(hwnd);
    if (!win) {
        void* first = window_manager().first_window();
        if (first) win = window_manager().window_from_hwnd(first);
        if (!win) {
            void* new_win = window_manager().create_window_ex("PapayaGame", "Buckshot Roulette", 0x10CF0000, 0, 0, 1280, 720, nullptr, nullptr, nullptr, false);
            win = window_manager().window_from_hwnd(new_win);
        }
    }
    void* dpy = win ? win->display : nullptr;
    uint64_t xid = win ? win->xid : 0;
    if (!dpy) {
        dpy = XOpenDisplay(nullptr);
    }

    typedef int (*vk_create_xlib_surface_fn)(void*, const VkXlibSurfaceCreateInfoKHR_T*, const void*, void**);
    static void* libvk = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!libvk) libvk = dlopen("libvulkan.so", RTLD_LAZY | RTLD_GLOBAL);
    static auto xlib_fn = reinterpret_cast<vk_create_xlib_surface_fn>(libvk ? dlsym(libvk, "vkCreateXlibSurfaceKHR") : nullptr);
    if (xlib_fn) {
        VkXlibSurfaceCreateInfoKHR_T xlib_info{};
        xlib_info.sType = 1000004000; // VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR
        xlib_info.dpy = dpy;
        xlib_info.window = xid;
        int res = xlib_fn(instance, &xlib_info, pAllocator, pSurface);
        log::info("VK_LAYER", "Intercepted vkCreateWin32SurfaceKHR -> vkCreateXlibSurfaceKHR [hwnd: {}, xid: 0x{:X}, res: {}]",
                  reinterpret_cast<u64>(hwnd), xid, res);
        return res;
    }
    return 0;
}

static PAPAYA_MS_ABI uint32_t hle_vk_get_physical_device_win32_presentation_support_khr(void* physicalDevice, uint32_t queueFamilyIndex) {
    (void)physicalDevice; (void)queueFamilyIndex;
    return 1; // VK_TRUE
}

__asm__(
".text\n"
".intel_syntax noprefix\n"
".globl ms_vulkan_bridge\n"
".type ms_vulkan_bridge, @function\n"
"ms_vulkan_bridge:\n"
"    push rbp\n"
"    mov rbp, rsp\n"
"    push rbx\n"
"    push rdi\n"
"    push rsi\n"
"    push r12\n"
"    push r13\n"
"    push r14\n"
"    push r15\n"
"    sub rsp, 264\n"
"    movdqu [rsp + 0x40], xmm6\n"
"    movdqu [rsp + 0x50], xmm7\n"
"    movdqu [rsp + 0x60], xmm8\n"
"    movdqu [rsp + 0x70], xmm9\n"
"    movdqu [rsp + 0x80], xmm10\n"
"    movdqu [rsp + 0x90], xmm11\n"
"    movdqu [rsp + 0xA0], xmm12\n"
"    movdqu [rsp + 0xB0], xmm13\n"
"    movdqu [rsp + 0xC0], xmm14\n"
"    movdqu [rsp + 0xD0], xmm15\n"
"    mov rax, [rbp + 0x40]\n"
"    mov [rsp + 0x00], rax\n"
"    mov rax, [rbp + 0x48]\n"
"    mov [rsp + 0x08], rax\n"
"    mov rax, [rbp + 0x50]\n"
"    mov [rsp + 0x10], rax\n"
"    mov rax, [rbp + 0x58]\n"
"    mov [rsp + 0x18], rax\n"
"    mov rax, [rbp + 0x60]\n"
"    mov [rsp + 0x20], rax\n"
"    mov rax, [rbp + 0x68]\n"
"    mov [rsp + 0x28], rax\n"
"    mov rax, [rbp + 0x30]\n"
"    mov r10, [rbp + 0x38]\n"
"    mov rdi, rcx\n"
"    mov rsi, rdx\n"
"    mov rdx, r8\n"
"    mov rcx, r9\n"
"    mov r8, rax\n"
"    mov r9, r10\n"
"    xor eax, eax\n"
"    call r11\n"
"    movdqu xmm6, [rsp + 0x40]\n"
"    movdqu xmm7, [rsp + 0x50]\n"
"    movdqu xmm8, [rsp + 0x60]\n"
"    movdqu xmm9, [rsp + 0x70]\n"
"    movdqu xmm10, [rsp + 0x80]\n"
"    movdqu xmm11, [rsp + 0x90]\n"
"    movdqu xmm12, [rsp + 0xA0]\n"
"    movdqu xmm13, [rsp + 0xB0]\n"
"    movdqu xmm14, [rsp + 0xC0]\n"
"    movdqu xmm15, [rsp + 0xD0]\n"
"    add rsp, 264\n"
"    pop r15\n"
"    pop r14\n"
"    pop r13\n"
"    pop r12\n"
"    pop rsi\n"
"    pop rdi\n"
"    pop rbx\n"
"    leave\n"
"    ret\n"
".att_syntax prefix\n"
);

extern "C" void ms_vulkan_bridge();

static void* get_vulkan_ms_thunk(void* host_fn) {
    if (!host_fn) return nullptr;
    static std::mutex s_thunk_mtx;
    std::lock_guard<std::mutex> lock(s_thunk_mtx);
    static std::unordered_map<void*, void*> s_thunks;
    static uint8_t* s_thunk_pool = nullptr;
    static size_t s_thunk_offset = 0;

    auto it = s_thunks.find(host_fn);
    if (it != s_thunks.end()) return it->second;

    if (!s_thunk_pool || s_thunk_offset + 32 > 4096 * 64) {
        s_thunk_pool = static_cast<uint8_t*>(mmap(nullptr, 4096 * 64,
            PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
        s_thunk_offset = 0;
    }

    uint8_t* code = s_thunk_pool + s_thunk_offset;
    s_thunk_offset += 32;

    // 49 BB <host_fn>          -> mov r11, host_fn (10 bytes)
    code[0] = 0x49; code[1] = 0xBB;
    std::memcpy(code + 2, &host_fn, 8);
    // 48 B8 <ms_vulkan_bridge> -> mov rax, ms_vulkan_bridge (10 bytes)
    code[10] = 0x48; code[11] = 0xB8;
    void* bridge_ptr = reinterpret_cast<void*>(&ms_vulkan_bridge);
    std::memcpy(code + 12, &bridge_ptr, 8);
    // FF E0                   -> jmp rax (2 bytes)
    code[20] = 0xFF; code[21] = 0xE0;
    code[22] = 0x90; // nop

    s_thunks[host_fn] = code;
    return code;
}

struct VkExtensionProperties_T {
    char extensionName[256];
    uint32_t specVersion;
};

static PAPAYA_MS_ABI int hle_vk_enumerate_instance_extension_properties(
    const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties_T* pProperties) {
    static void* libvk = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!libvk) libvk = dlopen("libvulkan.so", RTLD_LAZY | RTLD_GLOBAL);
    typedef int (*vk_enum_ext_fn)(const char*, uint32_t*, VkExtensionProperties_T*);
    static auto real_fn = reinterpret_cast<vk_enum_ext_fn>(libvk ? dlsym(libvk, "vkEnumerateInstanceExtensionProperties") : nullptr);
    if (!real_fn) return -1;

    if (!pProperties) {
        int res = real_fn(pLayerName, pPropertyCount, nullptr);
        if (res == 0 && pPropertyCount) (*pPropertyCount) += 2;
        return res;
    }

    uint32_t count = *pPropertyCount;
    int res = real_fn(pLayerName, &count, pProperties);
    if (res == 0) {
        bool has_win32 = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (std::strcmp(pProperties[i].extensionName, "VK_KHR_win32_surface") == 0) has_win32 = true;
        }
        if (!has_win32 && count < *pPropertyCount) {
            std::strncpy(pProperties[count].extensionName, "VK_KHR_win32_surface", 255);
            pProperties[count].specVersion = 6;
            count++;
        }
        *pPropertyCount = count;
    }
    return res;
}

struct VkInstanceCreateInfo_T {
    uint32_t sType;
    const void* pNext;
    uint32_t flags;
    const void* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};

static PAPAYA_MS_ABI int hle_vk_create_instance(
    const VkInstanceCreateInfo_T* pCreateInfo, const void* pAllocator, void** pInstance) {
    if (!pCreateInfo || !pInstance) return -1;
    static void* libvk = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!libvk) libvk = dlopen("libvulkan.so", RTLD_LAZY | RTLD_GLOBAL);
    typedef int (*vk_create_inst_fn)(const VkInstanceCreateInfo_T*, const void*, void**);
    static auto real_fn = reinterpret_cast<vk_create_inst_fn>(libvk ? dlsym(libvk, "vkCreateInstance") : nullptr);
    if (!real_fn) return -1;

    std::vector<const char*> ext_names;
    bool has_xlib = false;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        const char* name = pCreateInfo->ppEnabledExtensionNames[i];
        if (std::strcmp(name, "VK_KHR_win32_surface") == 0) {
            ext_names.push_back("VK_KHR_xlib_surface");
            has_xlib = true;
        } else {
            ext_names.push_back(name);
        }
    }
    if (!has_xlib) ext_names.push_back("VK_KHR_xlib_surface");

    VkInstanceCreateInfo_T modified_info = *pCreateInfo;
    modified_info.enabledExtensionCount = static_cast<uint32_t>(ext_names.size());
    modified_info.ppEnabledExtensionNames = ext_names.data();

    int res = real_fn(&modified_info, pAllocator, pInstance);
    log::info("VK_LAYER", "Intercepted vkCreateInstance -> res={}", res);
    return res;
}

static PAPAYA_MS_ABI void* hle_vk_get_device_proc_addr(void* device, const char* pName) {
    if (!pName || !device) return nullptr;
    static void* libvk = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!libvk) libvk = dlopen("libvulkan.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!libvk) return nullptr;
    typedef void* (*vk_get_dev_proc_fn)(void*, const char*);
    static auto dev_proc = reinterpret_cast<vk_get_dev_proc_fn>(dlsym(libvk, "vkGetDeviceProcAddr"));
    void* p = dev_proc ? dev_proc(device, pName) : nullptr;
    if (p) return get_vulkan_ms_thunk(p);
    return nullptr;
}

static PAPAYA_MS_ABI int hle_vk_enumerate_instance_version(uint32_t* pApiVersion) {
    if (pApiVersion) *pApiVersion = 0x00403000; // Vulkan 1.3
    return 0; // VK_SUCCESS
}

void* Win32ApiHle::hle_vk_get_instance_proc_addr(void* instance, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkCreateWin32SurfaceKHR") == 0) {
        return reinterpret_cast<void*>(&hle_vk_create_win32_surface_khr);
    }
    if (std::strcmp(pName, "vkGetPhysicalDeviceWin32PresentationSupportKHR") == 0) {
        return reinterpret_cast<void*>(&hle_vk_get_physical_device_win32_presentation_support_khr);
    }
    if (std::strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0) {
        return reinterpret_cast<void*>(&hle_vk_enumerate_instance_extension_properties);
    }
    if (std::strcmp(pName, "vkCreateInstance") == 0) {
        return reinterpret_cast<void*>(&hle_vk_create_instance);
    }
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<void*>(&hle_vk_get_device_proc_addr);
    }
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<void*>(&hle_vk_get_instance_proc_addr);
    }
    if (std::strcmp(pName, "vkEnumerateInstanceVersion") == 0) {
        return reinterpret_cast<void*>(&hle_vk_enumerate_instance_version);
    }

    static void* libvk = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!libvk) libvk = dlopen("libvulkan.so", RTLD_LAZY | RTLD_GLOBAL);
    if (libvk) {
        typedef void* (*vk_get_proc_fn)(void*, const char*);
        static auto vk_proc = reinterpret_cast<vk_get_proc_fn>(dlsym(libvk, "vkGetInstanceProcAddr"));
        void* p = vk_proc ? vk_proc(instance, pName) : nullptr;
        if (!p && !instance) p = dlsym(libvk, pName);
        if (p) return get_vulkan_ms_thunk(p);
    }
    return nullptr;
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
        char16_t szCSDVersion[128];
    };
    auto* vi = static_cast<Win32OsVersionInfoW*>(lpVersionInfo);
    vi->dwMajorVersion = 10;
    vi->dwMinorVersion = 0;
    vi->dwBuildNumber = 19041;
    vi->dwPlatformId = 2;
    vi->szCSDVersion[0] = 0;
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

int Win32ApiHle::hle_wsaconnect(u64 s, const void* name, int namelen, const void* lpCallerData,
                                void* lpCalleeData, const void* lpSQOS, const void* lpGQOS) {
    (void)lpCalleeData; (void)lpSQOS; (void)lpGQOS;
    int fd = static_cast<int>(s);
    int rc = ::connect(fd, static_cast<const struct sockaddr*>(name), static_cast<socklen_t>(namelen));
    if (rc != 0) return rc;   // SOCKET_ERROR (-1) as host connect failed
    // Deliver caller data (WSABUF: len@0, buf@8) if supplied.
    if (lpCallerData) {
        auto* wb = static_cast<const u64*>(lpCallerData);
        u32 len = static_cast<u32>(wb[0]);
        const void* buf = reinterpret_cast<const void*>(wb[1]);
        if (len && buf) ::send(fd, static_cast<const char*>(buf), len, 0);
    }
    return 0;
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

// ---- Winsock server + addressing (real host-socket wrappers) -----------------
int Win32ApiHle::hle_bind(u64 s, const void* addr, int addrlen) {
    return ::bind(static_cast<int>(s), static_cast<const struct sockaddr*>(addr), static_cast<socklen_t>(addrlen));
}
int Win32ApiHle::hle_listen(u64 s, int backlog) {
    return ::listen(static_cast<int>(s), backlog);
}
u64 Win32ApiHle::hle_accept(u64 s, void* addr, void* addrlen_ptr) {
    socklen_t alen = 0;
    int c = ::accept(static_cast<int>(s), static_cast<struct sockaddr*>(addr), addrlen_ptr ? &alen : nullptr);
    if (addrlen_ptr && addr) *static_cast<int*>(addrlen_ptr) = (int)alen;
    return (c >= 0) ? static_cast<u64>(c) : 0xFFFFFFFFFFFFFFFFULL;
}
int Win32ApiHle::hle_getsockname(u64 s, void* name, void* namelen_ptr) {
    socklen_t alen = 0;
    int r = ::getsockname(static_cast<int>(s), static_cast<struct sockaddr*>(name), &alen);
    if (namelen_ptr) *static_cast<int*>(namelen_ptr) = (int)alen;
    return r;
}
int Win32ApiHle::hle_getpeername(u64 s, void* name, void* namelen_ptr) {
    socklen_t alen = 0;
    int r = ::getpeername(static_cast<int>(s), static_cast<struct sockaddr*>(name), &alen);
    if (namelen_ptr) *static_cast<int*>(namelen_ptr) = (int)alen;
    return r;
}
int Win32ApiHle::hle_setsockopt(u64 s, int level, int optname, const void* optval, int optlen) {
    return ::setsockopt(static_cast<int>(s), level, optname, optval, static_cast<socklen_t>(optlen));
}
int Win32ApiHle::hle_shutdown(u64 s, int how) {
    return ::shutdown(static_cast<int>(s), how);
}
u32 Win32ApiHle::hle_inet_addr(const char* cp) {
    if (!cp) return 0xFFFFFFFF;   // INADDR_NONE on error
    struct in_addr a{};
    if (inet_pton(AF_INET, cp, &a) != 1) return 0xFFFFFFFF;
    return static_cast<u32>(a.s_addr);
}
const char* Win32ApiHle::hle_inet_ntoa(void* in_addr_ptr) {
    static thread_local char buf[INET_ADDRSTRLEN];
    if (!in_addr_ptr) return "0.0.0.0";
    struct in_addr a{}; a.s_addr = *static_cast<u32*>(in_addr_ptr);
    const char* r = inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return r ? r : "0.0.0.0";
}
int Win32ApiHle::hle_select(u32 nfds, void* rfds, void* wfds, void* efds, void* timeout) {
    return ::select(static_cast<int>(nfds), static_cast<fd_set*>(rfds), static_cast<fd_set*>(wfds),
                    static_cast<fd_set*>(efds), static_cast<struct timeval*>(timeout));
}

int Win32ApiHle::hle_getaddrinfo(const char* nodename, const char* servname, const void* hints, void** res) {
    int r = ::getaddrinfo(nodename, servname, static_cast<const struct addrinfo*>(hints),
                          reinterpret_cast<struct addrinfo**>(res));
    // Host EAI codes are used; games check for 0 (success) vs nonzero (name not found).
    return r;
}
void Win32ApiHle::hle_freeaddrinfo(void* res) {
    if (res) ::freeaddrinfo(static_cast<struct addrinfo*>(res));
}
int Win32ApiHle::hle_getnameinfo(const void* sa, u32 salen, char* host, u32 hostlen, char* serv, u32 servlen, u32 flags) {
    return ::getnameinfo(static_cast<const struct sockaddr*>(sa), salen, host, hostlen, serv, servlen,
                         static_cast<int>(flags));
}
int Win32ApiHle::hle_inet_pton(int af, const char* src, void* dst) {
    int r = ::inet_pton(af, src, dst);
    return r == 1 ? 1 : (r == 0 ? 0 : -1);   // 1 valid, 0 not valid, -1 af not supported
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
    if (!lpString) return FALSE_VAL;
    auto* win = window_manager().window_from_hwnd(hWnd);
    if (win && win->display && win->xid) {
        win->title = lpString;
        XStoreName(win->display, static_cast<Window>(win->xid), lpString);
        Atom utf8_string = XInternAtom(win->display, "UTF8_STRING", False);
        Atom net_wm_name = XInternAtom(win->display, "_NET_WM_NAME", False);
        XChangeProperty(win->display, static_cast<Window>(win->xid), net_wm_name, utf8_string, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(lpString), std::strlen(lpString));
        XFlush(win->display);
    }
    return TRUE_VAL;
}

BOOL Win32ApiHle::hle_set_window_text_w(HWND hWnd, const wchar_t* lpString) {
    if (!lpString) return FALSE_VAL;
    std::string s = win_utf16_to_utf8(lpString);
    return hle_set_window_text_a(hWnd, s.c_str());
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
        return static_cast<int>(win_copy_u16(lpString, u"Papaya Game", static_cast<size_t>(nMaxCount)));
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
// SEH & Exception Handling Runtime
// -------------------------------------------------------------
static PAPAYA_MS_ABI void* hle_rtl_lookup_function_entry(u64 ControlPc, u64* ImageBase, void* HistoryTable) {
    (void)HistoryTable;
    u64 img_base = seh_image_base();
    if (ImageBase) *ImageBase = img_base;
    u64 eh_rva = 0, scope_rva = 0;
    const void* uw = seh_find_unwind_info(ControlPc, img_base, &eh_rva, &scope_rva);
    return const_cast<void*>(uw);
}

static PAPAYA_MS_ABI void* hle_rtl_virtual_unwind(
    u32 HandlerType, u64 ImageBase, u64 ControlPc, void* FunctionEntry,
    void* ContextRecord, void** HandlerData, u64* EstablisherFrame, void* ContextPointers) {
    (void)HandlerType; (void)ImageBase; (void)ControlPc; (void)FunctionEntry; (void)ContextPointers;
    if (EstablisherFrame && ContextRecord) {
        auto* ctx = static_cast<GuestContext*>(ContextRecord);
        *EstablisherFrame = ctx->rsp;
    }
    if (HandlerData) *HandlerData = nullptr;
    return nullptr;
}

static PAPAYA_MS_ABI void hle_rtl_capture_context(void* ContextRecord) {
    if (!ContextRecord) return;
    auto* ctx = static_cast<GuestContext*>(ContextRecord);
    ctx->rip = reinterpret_cast<u64>(__builtin_return_address(0));
    ctx->rsp = reinterpret_cast<u64>(__builtin_frame_address(0));
}

static PAPAYA_MS_ABI void hle_rtl_unwind(void* TargetFrame, void* TargetIp, void* ExceptionRecord, void* ReturnValue) {
    (void)TargetFrame; (void)TargetIp; (void)ExceptionRecord; (void)ReturnValue;
}

// Universal MSVC longjmp / RtlUnwind / RtlUnwindEx assembly thunk
extern "C" void hle_rtl_unwind_ex_impl();
__asm__(
    ".global hle_rtl_unwind_ex_impl\n"
    "hle_rtl_unwind_ex_impl:\n"
    "testq %r8, %r8\n"
    "jz check_rva_fallback\n"
    "cmpl $0x80000026, (%r8)\n" // STATUS_LONGJUMP
    "je do_longjmp_unwind\n"
    "check_rva_fallback:\n"
    "movq (%rsp), %r10\n" // r10 = return address
    "movabsq $0x140000000, %r11\n"
    "subq %r11, %r10\n" // r10 = return address RVA
    "cmpq $0x2772659, %r10\n" // Godot RVA check
    "jne unknown_unwind\n"
    "do_longjmp_unwind:\n"
    "movq 0x48(%rsp), %rcx\n" // rcx = jmp_buf from caller stack
    "testq %rcx, %rcx\n"
    "jnz 1f\n"
    "testq %r8, %r8\n"
    "jz unknown_unwind\n"
    "movq 0x20(%r8), %rcx\n" // ExceptionInformation[0]
    "1:\n"
    "movq %r9, %rax\n"        // rax = ReturnValue (r9 is arg 4)
    "testq %rax, %rax\n"
    "jnz 2f\n"
    "movq $1, %rax\n"
    "2:\n"
    "movq 0x08(%rcx), %rbx\n"
    "movq 0x10(%rcx), %rsp\n"
    "movq 0x18(%rcx), %rbp\n"
    "movq 0x20(%rcx), %rsi\n"
    "movq 0x28(%rcx), %rdi\n"
    "movq 0x30(%rcx), %r12\n"
    "movq 0x38(%rcx), %r13\n"
    "movq 0x40(%rcx), %r14\n"
    "movq 0x48(%rcx), %r15\n"
    "movq 0x50(%rcx), %rdx\n"
    "movdqa 0x60(%rcx), %xmm6\n"
    "movdqa 0x70(%rcx), %xmm7\n"
    "movdqa 0x80(%rcx), %xmm8\n"
    "movdqa 0x90(%rcx), %xmm9\n"
    "movdqa 0xa0(%rcx), %xmm10\n"
    "movdqa 0xb0(%rcx), %xmm11\n"
    "movdqa 0xc0(%rcx), %xmm12\n"
    "movdqa 0xd0(%rcx), %xmm13\n"
    "movdqa 0xe0(%rcx), %xmm14\n"
    "movdqa 0xf0(%rcx), %xmm15\n"
    "jmp *%rdx\n" // Jump to TargetIp!
    "unknown_unwind:\n"
    "jmp hle_rtl_unwind_ex_fallback\n"
);

extern "C" u64 seh_image_base_asm() {
    return seh_image_base();
}

extern "C" void hle_rtl_unwind_ex_fallback(void* TargetFrame, void* TargetIp, void* ExceptionRecord, void* ReturnValue, void* ContextRecord, void* HistoryTable) {
    u64 ret_addr = reinterpret_cast<u64>(__builtin_return_address(0));
    log::warn("WIN32", "RtlUnwindEx fallback! ret_addr=0x{:X} (RVA 0x{:X}), TargetIp=0x{:X}", 
              ret_addr, ret_addr - seh_image_base(), reinterpret_cast<u64>(TargetIp));
}

// UCRT / Win32 additions
static PAPAYA_MS_ABI BOOL hle_wait_on_address(volatile void* Address, void* CompareAddress, size_t AddressSize, u32 dwMilliseconds) {
    (void)Address; (void)CompareAddress; (void)AddressSize;
    if (dwMilliseconds > 0 && dwMilliseconds != 0xFFFFFFFF) {
        usleep(std::min<u32>(dwMilliseconds, 5) * 1000);
    }
    return TRUE_VAL;
}
static PAPAYA_MS_ABI void hle_wake_by_address_all(void* Address) { (void)Address; }
static PAPAYA_MS_ABI void hle_wake_by_address_single(void* Address) { (void)Address; }

static PAPAYA_MS_ABI int hle_sendto(int s, const char* buf, int len, int flags, const void* to, int tolen) {
    ssize_t ret = ::sendto(s, buf, len, flags, static_cast<const struct sockaddr*>(to), static_cast<socklen_t>(tolen));
    return ret >= 0 ? static_cast<int>(ret) : -1;
}

static PAPAYA_MS_ABI int hle_recvfrom(int s, char* buf, int len, int flags, void* from, int* fromlen) {
    socklen_t flen = fromlen ? *fromlen : 0;
    ssize_t ret = ::recvfrom(s, buf, len, flags, static_cast<struct sockaddr*>(from), fromlen ? &flen : nullptr);
    if (fromlen) *fromlen = static_cast<int>(flen);
    return ret >= 0 ? static_cast<int>(ret) : -1;
}

static PAPAYA_MS_ABI char* hle_getenv(const char* varname) {
    return varname ? std::getenv(varname) : nullptr;
}

static PAPAYA_MS_ABI int hle_putenv_s(const char* name, const char* value) {
    if (!name) return -1;
    if (!value || value[0] == '\0') {
        unsetenv(name);
    } else {
        setenv(name, value, 1);
    }
    return 0;
}

static PAPAYA_MS_ABI wchar_t* hle_wgetcwd(wchar_t* buffer, int maxlen) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        if (!buffer) buffer = static_cast<wchar_t*>(std::malloc(sizeof(wchar_t) * 1024));
        mbstowcs(buffer, cwd, maxlen > 0 ? maxlen : 1024);
        return buffer;
    }
    return nullptr;
}

} // namespace papaya::win32

extern "C" char** environ;

namespace papaya::win32 {

static PAPAYA_MS_ABI char*** hle_p_environ() {
    static char** env_ptr = ::environ;
    return &env_ptr;
}

static PAPAYA_MS_ABI int64_t hle_time64(int64_t* destTime) {
    int64_t t = static_cast<int64_t>(time(nullptr));
    if (destTime) *destTime = t;
    return t;
}

static PAPAYA_MS_ABI struct tm* hle_gmtime64(const int64_t* sourceTime) {
    if (!sourceTime) return nullptr;
    time_t t = static_cast<time_t>(*sourceTime);
    return gmtime(&t);
}

static PAPAYA_MS_ABI int hle_gmtime64_s(struct tm* tmDest, const int64_t* sourceTime) {
    if (!tmDest || !sourceTime) return -1;
    time_t t = static_cast<time_t>(*sourceTime);
    return gmtime_r(&t, tmDest) ? 0 : -1;
}

static PAPAYA_MS_ABI size_t hle_strftime_l(char* strDest, size_t maxsize, const char* format, const struct tm* timeptr, void* locale) {
    (void)locale;
    return strftime(strDest, maxsize, format, timeptr);
}

static PAPAYA_MS_ABI void hle_tzset() {
    tzset();
}

static PAPAYA_MS_ABI int* hle_daylight() {
    static int d = 0;
    return &d;
}

static PAPAYA_MS_ABI long* hle_timezone() {
    static long tz = 0;
    return &tz;
}

static PAPAYA_MS_ABI char*** hle_tzname() {
    static char* names[2] = { const_cast<char*>("UTC"), const_cast<char*>("UTC") };
    static char** pnames = names;
    return &pnames;
}

static PAPAYA_MS_ABI void hle_qsort(void* base, size_t num, size_t width, int (*compare)(const void*, const void*)) {
    std::qsort(base, num, width, compare);
}

static PAPAYA_MS_ABI void* hle_bsearch(const void* key, const void* base, size_t num, size_t width, int (*compare)(const void*, const void*)) {
    return std::bsearch(key, base, num, width, compare);
}

static PAPAYA_MS_ABI int hle_rand() {
    return std::rand();
}

static PAPAYA_MS_ABI void hle_srand(unsigned int seed) {
    std::srand(seed);
}

static PAPAYA_MS_ABI int hle_mbtowc_l(wchar_t* wchar, const char* mbchar, size_t count, void* locale) {
    (void)locale;
    if (!mbchar || count == 0) return 0;
    if (wchar) *wchar = static_cast<wchar_t>(static_cast<unsigned char>(*mbchar));
    return 1;
}

static PAPAYA_MS_ABI char* hle_setlocale(int category, const char* locale) {
    return std::setlocale(category, locale ? locale : "C");
}
static PAPAYA_MS_ABI void* hle_create_locale(int category, const char* locale) {
    (void)category; (void)locale;
    static int dummy_locale = 1;
    return &dummy_locale;
}
static PAPAYA_MS_ABI void hle_free_locale(void* locale) {
    (void)locale;
}

static PAPAYA_MS_ABI double hle_strtod(const char* nptr, char** endptr) {
    return std::strtod(nptr, endptr);
}
static PAPAYA_MS_ABI float hle_strtof(const char* nptr, char** endptr) {
    return std::strtof(nptr, endptr);
}
static PAPAYA_MS_ABI long hle_strtol(const char* nptr, char** endptr, int base) {
    return std::strtol(nptr, endptr, base);
}
static PAPAYA_MS_ABI long long hle_strtoll(const char* nptr, char** endptr, int base) {
    return std::strtoll(nptr, endptr, base);
}
static PAPAYA_MS_ABI unsigned long hle_strtoul(const char* nptr, char** endptr, int base) {
    return std::strtoul(nptr, endptr, base);
}
static PAPAYA_MS_ABI unsigned long long hle_strtoull(const char* nptr, char** endptr, int base) {
    return std::strtoull(nptr, endptr, base);
}
static PAPAYA_MS_ABI double hle_wcstod(const wchar_t* nptr, wchar_t** endptr) {
    return std::wcstod(nptr, endptr);
}
static PAPAYA_MS_ABI long hle_wcstol(const wchar_t* nptr, wchar_t** endptr, int base) {
    return std::wcstol(nptr, endptr, base);
}
static PAPAYA_MS_ABI long long hle_wcstoll(const wchar_t* nptr, wchar_t** endptr, int base) {
    return std::wcstoll(nptr, endptr, base);
}
static PAPAYA_MS_ABI unsigned long hle_wcstoul(const wchar_t* nptr, wchar_t** endptr, int base) {
    return std::wcstoul(nptr, endptr, base);
}
static PAPAYA_MS_ABI unsigned long long hle_wcstoull(const wchar_t* nptr, wchar_t** endptr, int base) {
    return std::wcstoull(nptr, endptr, base);
}
static PAPAYA_MS_ABI double hle_strtod_l(const char* nptr, char** endptr, void* loc) {
    (void)loc; return std::strtod(nptr, endptr);
}
static PAPAYA_MS_ABI int64_t hle_strtoi64_l(const char* nptr, char** endptr, int base, void* loc) {
    (void)loc; return std::strtoll(nptr, endptr, base);
}
static PAPAYA_MS_ABI uint64_t hle_strtoui64_l(const char* nptr, char** endptr, int base, void* loc) {
    (void)loc; return std::strtoull(nptr, endptr, base);
}
static PAPAYA_MS_ABI int hle_itoa_s(int val, char* buf, size_t sz, int radix) {
    if (!buf || sz == 0) return -1;
    if (radix == 10) std::snprintf(buf, sz, "%d", val);
    else if (radix == 16) std::snprintf(buf, sz, "%x", val);
    else if (radix == 8) std::snprintf(buf, sz, "%o", val);
    else std::snprintf(buf, sz, "%d", val);
    return 0;
}
static PAPAYA_MS_ABI int hle_wctob(wint_t c) { return wctob(c); }
static PAPAYA_MS_ABI wint_t hle_btowc(int c) { return btowc(c); }
static PAPAYA_MS_ABI size_t hle_mbrtowc(wchar_t* pwc, const char* s, size_t n, void* ps) {
    return std::mbrtowc(pwc, s, n, static_cast<mbstate_t*>(ps));
}
static PAPAYA_MS_ABI size_t hle_wcrtomb(char* s, wchar_t wc, void* ps) {
    return std::wcrtomb(s, wc, static_cast<mbstate_t*>(ps));
}
static PAPAYA_MS_ABI int hle_wcrtomb_s(size_t* pRetVal, char* dst, size_t dstSizeInBytes, wchar_t wc, void* ps) {
    (void)dstSizeInBytes;
    size_t res = std::wcrtomb(dst, wc, static_cast<mbstate_t*>(ps));
    if (pRetVal) *pRetVal = res;
    return 0;
}
static PAPAYA_MS_ABI size_t hle_mbsrtowcs(wchar_t* dst, const char** src, size_t len, void* ps) {
    return std::mbsrtowcs(dst, src, len, static_cast<mbstate_t*>(ps));
}

static PAPAYA_MS_ABI void* hle_aligned_malloc(size_t size, size_t alignment) {
    void* ptr = nullptr;
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    if (posix_memalign(&ptr, alignment, size) != 0) return nullptr;
    return ptr;
}
static PAPAYA_MS_ABI void hle_aligned_free(void* ptr) {
    if (ptr) std::free(ptr);
}
static PAPAYA_MS_ABI int hle_set_new_mode(int newMode) { (void)newMode; return 0; }

static PAPAYA_MS_ABI const unsigned short* hle_pctype_func() {
    static unsigned short pctype[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) {
            unsigned short mask = 0;
            if (std::isalpha(i)) mask |= 0x0103;
            if (std::isdigit(i)) mask |= 0x0004;
            if (std::isspace(i)) mask |= 0x0008;
            if (std::ispunct(i)) mask |= 0x0010;
            if (std::iscntrl(i)) mask |= 0x0020;
            pctype[i] = mask;
        }
        init = true;
    }
    return pctype;
}
static PAPAYA_MS_ABI int hle_configthreadlocale(int per_thread_locale_type) {
    return per_thread_locale_type;
}

static PAPAYA_MS_ABI void* hle_memchr(const void* s, int c, size_t n) { return const_cast<void*>(std::memchr(s, c, n)); }
static PAPAYA_MS_ABI int hle_memcmp(const void* s1, const void* s2, size_t n) { return std::memcmp(s1, s2, n); }
static PAPAYA_MS_ABI char* hle_strchr(const char* s, int c) { return const_cast<char*>(std::strchr(s, c)); }
static PAPAYA_MS_ABI char* hle_strrchr(const char* s, int c) { return const_cast<char*>(std::strrchr(s, c)); }
static PAPAYA_MS_ABI char* hle_strstr(const char* haystack, const char* needle) { return const_cast<char*>(std::strstr(haystack, needle)); }

// Math functions
static PAPAYA_MS_ABI double hle_sin(double x) { return std::sin(x); }
static PAPAYA_MS_ABI float  hle_sinf(float x) { return std::sin(x); }
static PAPAYA_MS_ABI double hle_cos(double x) { return std::cos(x); }
static PAPAYA_MS_ABI float  hle_cosf(float x) { return std::cos(x); }
static PAPAYA_MS_ABI double hle_tan(double x) { return std::tan(x); }
static PAPAYA_MS_ABI float  hle_tanf(float x) { return std::tan(x); }
static PAPAYA_MS_ABI double hle_sinh(double x) { return std::sinh(x); }
static PAPAYA_MS_ABI double hle_cosh(double x) { return std::cosh(x); }
static PAPAYA_MS_ABI double hle_tanh(double x) { return std::tanh(x); }
static PAPAYA_MS_ABI float  hle_tanhf(float x) { return std::tanh(x); }
static PAPAYA_MS_ABI double hle_sqrt(double x) { return std::sqrt(x); }
static PAPAYA_MS_ABI float  hle_sqrtf(float x) { return std::sqrt(x); }
static PAPAYA_MS_ABI double hle_pow(double x, double y) { return std::pow(x, y); }
static PAPAYA_MS_ABI float  hle_powf(float x, float y) { return std::pow(x, y); }
static PAPAYA_MS_ABI double hle_log(double x) { return std::log(x); }
static PAPAYA_MS_ABI float  hle_logf(float x) { return std::log(x); }
static PAPAYA_MS_ABI double hle_exp(double x) { return std::exp(x); }
static PAPAYA_MS_ABI float  hle_expf(float x) { return std::exp(x); }
static PAPAYA_MS_ABI double hle_floor(double x) { return std::floor(x); }
static PAPAYA_MS_ABI float  hle_floorf(float x) { return std::floor(x); }
static PAPAYA_MS_ABI double hle_ceil(double x) { return std::ceil(x); }
static PAPAYA_MS_ABI float  hle_ceilf(float x) { return std::ceil(x); }
static PAPAYA_MS_ABI double hle_fabs(double x) { return std::fabs(x); }
static PAPAYA_MS_ABI float  hle_fabsf(float x) { return std::fabs(x); }
static PAPAYA_MS_ABI double hle_atan(double x) { return std::atan(x); }
static PAPAYA_MS_ABI float  hle_atanf(float x) { return std::atan(x); }
static PAPAYA_MS_ABI double hle_atan2(double y, double x) { return std::atan2(y, x); }
static PAPAYA_MS_ABI float  hle_atan2f(float y, float x) { return std::atan2(y, x); }
static PAPAYA_MS_ABI double hle_asin(double x) { return std::asin(x); }
static PAPAYA_MS_ABI float  hle_asinf(float x) { return std::asin(x); }
static PAPAYA_MS_ABI double hle_acos(double x) { return std::acos(x); }
static PAPAYA_MS_ABI float  hle_acosf(float x) { return std::acos(x); }
static PAPAYA_MS_ABI double hle_fmod(double x, double y) { return std::fmod(x, y); }
static PAPAYA_MS_ABI float  hle_fmodf(float x, float y) { return std::fmod(x, y); }
static PAPAYA_MS_ABI double hle_modf(double x, double* iptr) { return std::modf(x, iptr); }
static PAPAYA_MS_ABI float  hle_modff(float x, float* iptr) { return std::modf(x, iptr); }
static PAPAYA_MS_ABI double hle_remainder(double x, double y) { return std::remainder(x, y); }
static PAPAYA_MS_ABI double hle_remquo(double x, double y, int* quo) { return std::remquo(x, y, quo); }
static PAPAYA_MS_ABI double hle_nextafter(double x, double y) { return std::nextafter(x, y); }
static PAPAYA_MS_ABI long   hle_lrintf(float x) { return std::lrint(x); }
static PAPAYA_MS_ABI double hle_log10(double x) { return std::log10(x); }
static PAPAYA_MS_ABI float  hle_log10f(float x) { return std::log10(x); }
static PAPAYA_MS_ABI double hle_log2(double x) { return std::log2(x); }
static PAPAYA_MS_ABI float  hle_log2f(float x) { return std::log2(x); }
static PAPAYA_MS_ABI double hle_hypot(double x, double y) { return std::hypot(x, y); }
static PAPAYA_MS_ABI float  hle_hypotf(float x, float y) { return std::hypot(x, y); }
static PAPAYA_MS_ABI double hle_cbrt(double x) { return std::cbrt(x); }
static PAPAYA_MS_ABI float  hle_cbrtf(float x) { return std::cbrt(x); }
static PAPAYA_MS_ABI double hle_exp2(double x) { return std::exp2(x); }
static PAPAYA_MS_ABI float  hle_exp2f(float x) { return std::exp2(x); }
static PAPAYA_MS_ABI double hle_fma(double x, double y, double z) { return std::fma(x, y, z); }
static PAPAYA_MS_ABI float  hle_fmaf(float x, float y, float z) { return std::fma(x, y, z); }
static PAPAYA_MS_ABI double hle_fmax(double x, double y) { return std::fmax(x, y); }
static PAPAYA_MS_ABI float  hle_fmaxf(float x, float y) { return std::fmax(x, y); }
static PAPAYA_MS_ABI double hle_fmin(double x, double y) { return std::fmin(x, y); }
static PAPAYA_MS_ABI float  hle_fminf(float x, float y) { return std::fmin(x, y); }
static PAPAYA_MS_ABI double hle_frexp(double x, int* exp) { return std::frexp(x, exp); }
static PAPAYA_MS_ABI int    hle_ilogb(double x) { return std::ilogb(x); }
static PAPAYA_MS_ABI long long hle_llrintf(float x) { return std::llrint(x); }
static PAPAYA_MS_ABI double hle_acosh(double x) { return std::acosh(x); }
static PAPAYA_MS_ABI double hle_asinh(double x) { return std::asinh(x); }
static PAPAYA_MS_ABI double hle_atanh(double x) { return std::atanh(x); }

// Wide String functions
static PAPAYA_MS_ABI size_t hle_wcslen(const wchar_t* s) { return s ? std::wcslen(s) : 0; }
static PAPAYA_MS_ABI size_t hle_wcsnlen(const wchar_t* s, size_t maxlen) {
    if (!s) return 0;
    size_t i = 0;
    while (i < maxlen && s[i]) i++;
    return i;
}
static PAPAYA_MS_ABI int hle_wcscmp(const wchar_t* s1, const wchar_t* s2) { return std::wcscmp(s1, s2); }
static PAPAYA_MS_ABI int hle_wcsncmp(const wchar_t* s1, const wchar_t* s2, size_t n) { return std::wcsncmp(s1, s2, n); }
static PAPAYA_MS_ABI wchar_t* hle_wcscpy(wchar_t* dest, const wchar_t* src) { return std::wcscpy(dest, src); }
static PAPAYA_MS_ABI wchar_t* hle_wcsncpy(wchar_t* dest, const wchar_t* src, size_t n) { return std::wcsncpy(dest, src, n); }
static PAPAYA_MS_ABI int hle_wcscpy_s(wchar_t* dest, size_t destSz, const wchar_t* src) {
    if (!dest || !src || destSz == 0) return -1;
    std::wcsncpy(dest, src, destSz);
    dest[destSz - 1] = 0;
    return 0;
}
static PAPAYA_MS_ABI wchar_t* hle_wcsstr(const wchar_t* haystack, const wchar_t* needle) { return const_cast<wchar_t*>(std::wcsstr(haystack, needle)); }
static PAPAYA_MS_ABI wchar_t* hle_wcschr(const wchar_t* s, wchar_t c) { return const_cast<wchar_t*>(std::wcschr(s, c)); }
static PAPAYA_MS_ABI wchar_t* hle_wcsrchr(const wchar_t* s, wchar_t c) { return const_cast<wchar_t*>(std::wcsrchr(s, c)); }

static PAPAYA_MS_ABI int hle_isalnum(int c) { return std::isalnum(c); }
static PAPAYA_MS_ABI int hle_isalpha(int c) { return std::isalpha(c); }
static PAPAYA_MS_ABI int hle_ispunct(int c) { return std::ispunct(c); }
static PAPAYA_MS_ABI int hle_isspace(int c) { return std::isspace(c); }
static PAPAYA_MS_ABI int hle_isxdigit(int c) { return std::isxdigit(c); }
static PAPAYA_MS_ABI int hle_tolower(int c) { return std::tolower(c); }
static PAPAYA_MS_ABI int hle_toupper(int c) { return std::toupper(c); }
static PAPAYA_MS_ABI int hle_tolower_l(int c, void* loc) { (void)loc; return std::tolower(c); }
static PAPAYA_MS_ABI int hle_toupper_l(int c, void* loc) { (void)loc; return std::toupper(c); }
static PAPAYA_MS_ABI wint_t hle_towlower_l(wint_t c, void* loc) { (void)loc; return std::towlower(c); }
static PAPAYA_MS_ABI wint_t hle_towupper_l(wint_t c, void* loc) { (void)loc; return std::towupper(c); }
static PAPAYA_MS_ABI int hle_iswxdigit_l(wint_t c, void* loc) { (void)loc; return std::iswxdigit(c); }
static PAPAYA_MS_ABI char* hle_strdup(const char* s) { return s ? strdup(s) : nullptr; }
static PAPAYA_MS_ABI int hle_stricmp(const char* s1, const char* s2) { return strcasecmp(s1, s2); }
static PAPAYA_MS_ABI int hle_memicmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* u1 = static_cast<const unsigned char*>(s1);
    const unsigned char* u2 = static_cast<const unsigned char*>(s2);
    for (size_t i = 0; i < n; ++i) {
        int diff = std::tolower(u1[i]) - std::tolower(u2[i]);
        if (diff != 0) return diff;
    }
    return 0;
}
static PAPAYA_MS_ABI int hle_strcoll_l(const char* s1, const char* s2, void* loc) { (void)loc; return std::strcoll(s1, s2); }
static PAPAYA_MS_ABI int hle_wcscoll_l(const wchar_t* s1, const wchar_t* s2, void* loc) { (void)loc; return std::wcscoll(s1, s2); }
static PAPAYA_MS_ABI size_t hle_strxfrm_l(char* dest, const char* src, size_t n, void* loc) { (void)loc; return std::strxfrm(dest, src, n); }
static PAPAYA_MS_ABI size_t hle_wcsxfrm_l(wchar_t* dest, const wchar_t* src, size_t n, void* loc) { (void)loc; return std::wcsxfrm(dest, src, n); }
static PAPAYA_MS_ABI int hle_strcpy_s(char* dest, size_t destSz, const char* src) {
    if (!dest || !src || destSz == 0) return -1;
    std::strncpy(dest, src, destSz);
    dest[destSz - 1] = '\0';
    return 0;
}
static PAPAYA_MS_ABI int hle_strcat_s(char* dest, size_t destSz, const char* src) {
    if (!dest || !src || destSz == 0) return -1;
    size_t dlen = std::strlen(dest);
    if (dlen >= destSz) return -1;
    std::strncat(dest, src, destSz - dlen - 1);
    return 0;
}
static PAPAYA_MS_ABI char* hle_strncat(char* dest, const char* src, size_t count) { return std::strncat(dest, src, count); }
static PAPAYA_MS_ABI size_t hle_strcspn(const char* s1, const char* s2) { return std::strcspn(s1, s2); }
static PAPAYA_MS_ABI char* hle_strpbrk(const char* s1, const char* s2) { return const_cast<char*>(std::strpbrk(s1, s2)); }
static PAPAYA_MS_ABI size_t hle_strnlen(const char* s, size_t maxlen) {
    if (!s) return 0;
    size_t i = 0;
    while (i < maxlen && s[i]) i++;
    return i;
}
static PAPAYA_MS_ABI size_t hle_mbrlen(const char* s, size_t n, void* ps) {
    return std::mbrlen(s, n, static_cast<mbstate_t*>(ps));
}

// UCRT runtime
static PAPAYA_MS_ABI int hle_configure_narrow_argv(int mode) { return mode; }
static PAPAYA_MS_ABI int hle_initialize_narrow_environment() { return 0; }
static PAPAYA_MS_ABI int hle_initterm_e(void** first, void** last) {
    if (!first || !last) return 0;
    for (void** cur = first; cur < last; ++cur) {
        if (*cur) {
            auto fn = reinterpret_cast<int (*)()>(*cur);
            int res = fn();
            if (res != 0) return res;
        }
    }
    return 0;
}
static PAPAYA_MS_ABI int hle_crt_atexit(void (*fn)()) {
    (void)fn;
    return 0;
}
static PAPAYA_MS_ABI void hle_register_thread_local_exe_atexit_callback(void* cb) { (void)cb; }
static PAPAYA_MS_ABI int hle_set_error_mode(int mode) { return mode; }
static PAPAYA_MS_ABI void* hle_set_invalid_parameter_handler(void* handler) { return handler; }
static PAPAYA_MS_ABI void hle_endthreadex(u32 retval) { (void)retval; pthread_exit(nullptr); }
static PAPAYA_MS_ABI int hle_getpid() { return getpid(); }
static PAPAYA_MS_ABI void hle_perror(const char* msg) { if (msg) perror(msg); }
static PAPAYA_MS_ABI int hle_strerror_s(char* buf, size_t sz, int errnum) {
    if (!buf || sz == 0) return -1;
    const char* str = strerror(errnum);
    if (str) std::strncpy(buf, str, sz);
    else std::snprintf(buf, sz, "Unknown error %d", errnum);
    buf[sz - 1] = 0;
    return 0;
}

// Filesystem
static PAPAYA_MS_ABI int hle_fstat64(int fd, void* statbuf) { (void)fd; (void)statbuf; return 0; }
static PAPAYA_MS_ABI void hle_lock_file(void* stream) { (void)stream; }
static PAPAYA_MS_ABI void hle_unlock_file(void* stream) { (void)stream; }
static PAPAYA_MS_ABI int hle_wchdir(const wchar_t* path) {
    if (!path) return -1;
    char mb[1024];
    wcstombs(mb, path, sizeof(mb));
    return chdir(mb);
}
static PAPAYA_MS_ABI int hle_remove(const char* filename) { return filename ? std::remove(filename) : -1; }

// isw* helpers
static PAPAYA_MS_ABI int hle_isctype_l(int c, unsigned int mask, void* loc) {
    (void)loc;
    return (hle_pctype_func()[static_cast<unsigned char>(c)] & mask) ? 1 : 0;
}
static PAPAYA_MS_ABI int hle_iswalpha_l(wint_t c, void* loc) { (void)loc; return iswalpha(c); }
static PAPAYA_MS_ABI int hle_iswcntrl_l(wint_t c, void* loc) { (void)loc; return iswcntrl(c); }
static PAPAYA_MS_ABI int hle_iswdigit_l(wint_t c, void* loc) { (void)loc; return iswdigit(c); }
static PAPAYA_MS_ABI int hle_iswlower_l(wint_t c, void* loc) { (void)loc; return iswlower(c); }
static PAPAYA_MS_ABI int hle_iswprint_l(wint_t c, void* loc) { (void)loc; return iswprint(c); }
static PAPAYA_MS_ABI int hle_iswpunct_l(wint_t c, void* loc) { (void)loc; return iswpunct(c); }
static PAPAYA_MS_ABI int hle_iswspace_l(wint_t c, void* loc) { (void)loc; return iswspace(c); }
static PAPAYA_MS_ABI int hle_iswupper_l(wint_t c, void* loc) { (void)loc; return iswupper(c); }

static int g_guest_argc = 1;
static char* g_guest_argv_storage[2] = { const_cast<char*>("SlayTheSpire2.exe"), nullptr };
static char** g_guest_argv = g_guest_argv_storage;

static PAPAYA_MS_ABI int* hle_p___argc() { return &g_guest_argc; }
static PAPAYA_MS_ABI char*** hle_p___argv() { return &g_guest_argv; }
static PAPAYA_MS_ABI int* hle_sys_nerr() { static int nerr = 32; return &nerr; }
static PAPAYA_MS_ABI void hle_assert(const char* msg, const char* file, unsigned line) {
    (void)msg; (void)file; (void)line;
}
static PAPAYA_MS_ABI uintptr_t hle_beginthreadex(void* sec, unsigned stack_size, unsigned (*start_address)(void*), void* arglist, unsigned initflag, unsigned* thrdaddr) {
    HANDLE h = Win32ApiHle::hle_create_thread(sec, stack_size, reinterpret_cast<void*>(start_address), arglist, initflag, thrdaddr);
    return reinterpret_cast<uintptr_t>(h);
}

// UCRT (ucrtbase.dll) compatibility family. Modern games (Unity, Godot Mono,
// .NET apps) import these directly under ucrtbase.dll rather than msvcrt.dll;
// the PE resolver forwards unknown ucrtbase imports here. All host-backed.
// ASCII-only case fold: locale-independent and deterministic, matching what
// games' wide-string compare/split paths need (avoiding std::towlower, which is
// locale- and stdlib-version-dependent and can silently no-op under a C locale).
static wchar_t* g_guest_wargv_storage[2] = { const_cast<wchar_t*>(L"game.exe"), nullptr };
static wchar_t** g_guest_wargv = g_guest_wargv_storage;

static PAPAYA_MS_ABI wchar_t*** hle_p___wargv() { return &g_guest_wargv; }
static PAPAYA_MS_ABI int hle_configure_wide_argv(int mode) { (void)mode; return 0; }
static PAPAYA_MS_ABI wchar_t** hle_get_initial_wide_environment() { static wchar_t* e[] = { nullptr }; return e; }
static PAPAYA_MS_ABI wchar_t** hle_initialize_wide_environment() { static wchar_t* e[] = { nullptr }; return e; }
static PAPAYA_MS_ABI char** hle_get_initial_narrow_environment() { static char* e[] = { nullptr }; return e; }

// Guest wide strings are UTF-16LE (2-byte code units, per the Windows ABI),
// but host wchar_t is 4-byte UTF-32. Reading a guest string as wchar_t* would
// misinterpret every code unit. All wide-string HLEs below therefore treat the
// pointer as const uint16_t* (UTF-16LE) and operate on code units directly.
static size_t u16_len(const uint16_t* s) { size_t n = 0; while (s && s[n]) ++n; return n; }
static uint16_t u16_lower(uint16_t c) { return (c >= 'A' && c <= 'Z') ? (uint16_t)(c + 32) : c; }
static uint16_t u16_upper(uint16_t c) { return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : c; }

static PAPAYA_MS_ABI int hle_wcsicmp(const void* a, const void* b) {
    const uint16_t* A = static_cast<const uint16_t*>(a);
    const uint16_t* B = static_cast<const uint16_t*>(b);
    for (;;) { uint16_t ca = u16_lower(*A), cb = u16_lower(*B); if (ca != cb || ca == 0) return (int)ca - (int)cb; ++A; ++B; }
}
static PAPAYA_MS_ABI void* hle_wcsdup(const void* s) {
    const uint16_t* S = static_cast<const uint16_t*>(s);
    if (!S) return nullptr;
    size_t n = u16_len(S) + 1;
    uint16_t* d = static_cast<uint16_t*>(malloc(n * sizeof(uint16_t)));
    if (d) for (size_t i = 0; i < n; ++i) d[i] = S[i];
    return d;
}
static PAPAYA_MS_ABI void* hle_wcscat(void* d, const void* s) {
    uint16_t* D = static_cast<uint16_t*>(d);
    const uint16_t* S = static_cast<const uint16_t*>(s);
    size_t dl = u16_len(D), sl = u16_len(S), i;
    for (i = 0; i < sl; ++i) D[dl + i] = S[i];
    D[dl + sl] = 0;
    return D;
}
static PAPAYA_MS_ABI int hle_wcsnicmp(const void* a, const void* b, size_t n) {
    const uint16_t* A = static_cast<const uint16_t*>(a);
    const uint16_t* B = static_cast<const uint16_t*>(b);
    for (size_t i = 0; i < n; ++i) { uint16_t ca = u16_lower(A[i]), cb = u16_lower(B[i]); if (ca != cb) return (int)ca - (int)cb; if (ca == 0) return 0; }
    return 0;
}
static PAPAYA_MS_ABI void* hle_wcspbrk(const void* s, const void* set) {
    const uint16_t* S = static_cast<const uint16_t*>(s);
    const uint16_t* Set = static_cast<const uint16_t*>(set);
    size_t sl = u16_len(Set);
    for (const uint16_t* p = S; *p; ++p) for (size_t i = 0; i < sl; ++i) if (*p == Set[i]) return const_cast<uint16_t*>(p);
    return nullptr;
}
static PAPAYA_MS_ABI int hle_iswspace(wint_t c) { wchar_t w = static_cast<wchar_t>(c); return (w == L' ' || w == L'\t' || w == L'\n' || w == L'\r' || w == L'\v' || w == L'\f') ? 1 : 0; }
static PAPAYA_MS_ABI wint_t hle_towupper(wint_t c) { return u16_upper(static_cast<uint16_t>(c)); }
static PAPAYA_MS_ABI wint_t hle_towlower(wint_t c) { return u16_lower(static_cast<uint16_t>(c)); }
static PAPAYA_MS_ABI int hle_iswprint(wint_t c) { wchar_t w = static_cast<wchar_t>(c); return (w >= 0x20 && w != 0x7f) ? 1 : 0; }
static PAPAYA_MS_ABI int hle_iswxdigit(wint_t c) { wchar_t w = static_cast<wchar_t>(c); return (w >= L'0' && w <= L'9') || (w >= L'a' && w <= L'f') || (w >= L'A' && w <= L'F') ? 1 : 0; }
static PAPAYA_MS_ABI int hle_iswdigit(wint_t c) { wchar_t w = static_cast<wchar_t>(c); return (w >= L'0' && w <= L'9') ? 1 : 0; }
static PAPAYA_MS_ABI int hle_iswalnum(wint_t c) { wchar_t w = static_cast<wchar_t>(c); return ((w >= L'A' && w <= L'Z') || (w >= L'a' && w <= L'z') || (w >= L'0' && w <= L'9')) ? 1 : 0; }
static PAPAYA_MS_ABI int hle_isprint(int c) { return (c >= 0x20 && c != 0x7f) ? 1 : 0; }

static PAPAYA_MS_ABI void* hle_wcsupr(void* s) { uint16_t* p = static_cast<uint16_t*>(s); if (p) for (; *p; ++p) *p = u16_upper(*p); return s; }
static PAPAYA_MS_ABI void* hle_wcslwr(void* s) { uint16_t* p = static_cast<uint16_t*>(s); if (p) for (; *p; ++p) *p = u16_lower(*p); return s; }
static PAPAYA_MS_ABI void* hle_wcsrev(void* s) {
    uint16_t* S = static_cast<uint16_t*>(s);
    if (!S) return nullptr;
    size_t n = u16_len(S); if (n < 2) return S;
    for (size_t i = 0, j = n - 1; i < j; ++i, --j) { uint16_t t = S[i]; S[i] = S[j]; S[j] = t; }
    return S;
}
static PAPAYA_MS_ABI void* hle_wcstok(void* s, const void* delim, void** ctx) {
    uint16_t* S = static_cast<uint16_t*>(s);
    const uint16_t* delimv = static_cast<const uint16_t*>(delim);
    uint16_t* cur = S;
    if (!cur && ctx) { uint16_t* saved = nullptr; std::memcpy(&saved, ctx, sizeof(saved)); cur = saved; }
    if (!cur) return nullptr;
    size_t dl = u16_len(delimv);
    while (*cur) { bool sep = false; for (size_t i = 0; i < dl; ++i) if (*cur == delimv[i]) { sep = true; break; } if (!sep) break; ++cur; }
    if (!*cur) { if (ctx) { uint16_t* n = nullptr; std::memcpy(ctx, &n, sizeof(n)); } return nullptr; }
    uint16_t* token = cur;
    while (*cur) { bool sep = false; for (size_t i = 0; i < dl; ++i) if (*cur == delimv[i]) { sep = true; break; } if (sep) break; ++cur; }
    if (*cur) { *cur = 0; ++cur; }
    if (ctx) std::memcpy(ctx, &cur, sizeof(cur));
    return token;
}
static PAPAYA_MS_ABI void* hle_wcstok_s(void* s, const void* delim, void** ctx) { return hle_wcstok(s, delim, ctx); }
static PAPAYA_MS_ABI size_t hle_wcscspn(const void* s, const void* set) {
    const uint16_t* S = static_cast<const uint16_t*>(s);
    const uint16_t* Set = static_cast<const uint16_t*>(set);
    size_t sl = u16_len(Set);
    size_t i = 0;
    for (; S[i]; ++i) for (size_t j = 0; j < sl; ++j) if (S[i] == Set[j]) return i;
    return i;
}
static PAPAYA_MS_ABI size_t hle_wcsspn(const void* s, const void* set) {
    const uint16_t* S = static_cast<const uint16_t*>(s);
    const uint16_t* Set = static_cast<const uint16_t*>(set);
    size_t sl = u16_len(Set), i = 0;
    while (S[i]) { bool in = false; for (size_t j = 0; j < sl; ++j) if (S[i] == Set[j]) { in = true; break; } if (!in) break; ++i; }
    return i;
}
static PAPAYA_MS_ABI int hle_wcsncat_s(void* d, size_t sz, const void* s, size_t n) {
    uint16_t* D = static_cast<uint16_t*>(d);
    const uint16_t* S = static_cast<const uint16_t*>(s);
    if (!D || !S || sz == 0) return -1;
    size_t dl = u16_len(D);
    size_t wn = u16_len(S); if (wn > n) wn = n;
    if (dl + wn + 1 >= sz) return -2;
    for (size_t i = 0; i < wn; ++i) D[dl + i] = S[i];
    D[dl + wn] = 0;
    return 0;
}
static PAPAYA_MS_ABI int hle_wcsncpy_s(void* d, size_t sz, const void* s, size_t n) {
    uint16_t* D = static_cast<uint16_t*>(d);
    const uint16_t* S = static_cast<const uint16_t*>(s);
    if (!D || !S || sz == 0) return -1;
    size_t src = u16_len(S); if (src > n) src = n;
    if (src >= sz) return -2;
    for (size_t i = 0; i < src; ++i) D[i] = S[i];
    D[src] = 0;
    return 0;
}
static PAPAYA_MS_ABI void hle_wsplitpath(const void* path, void* d, void* dir, void* fname, void* ext) {
    const uint16_t* P = static_cast<const uint16_t*>(path);
    if (!P) return;
    size_t len = u16_len(P);
    if (len > 2048) len = 2048;

    // Split drive (X:), dir, base name, extension by scanning back for last
    // slash and last dot. Copies UTF-16 code units into guest buffers.
    size_t drive_len = 0;
    size_t path_start = 0;
    if (len >= 2 && P[0] >= 'A' && P[0] <= 'Z' && P[1] == ':') { drive_len = 2; path_start = 2; }

    // find last slash among remaining
    size_t last_slash = (size_t)-1;
    for (size_t i = path_start; i < len; ++i) if (P[i] == L'/' || P[i] == L'\\') last_slash = i;
    size_t dir_start = path_start, dir_end = 0, name_start = path_start;
    if (last_slash != (size_t)-1) { dir_end = last_slash; name_start = last_slash + 1; }

    // find last dot in name
    size_t dot = (size_t)-1;
    for (size_t i = name_start; i < len; ++i) if (P[i] == L'.') dot = i;
    size_t base_end = (dot != (size_t)-1) ? dot : len;
    size_t ext_start = (dot != (size_t)-1) ? dot : len;

    auto copy = [](uint16_t* out, const uint16_t* src, size_t n) {
        if (!out) return;
        for (size_t i = 0; i < n; ++i) out[i] = src[i];
        out[n] = 0;
    };
    copy(static_cast<uint16_t*>(d), P, drive_len);
    copy(static_cast<uint16_t*>(dir), P + dir_start, dir_end > dir_start ? dir_end - dir_start + 1 : 0);
    copy(static_cast<uint16_t*>(fname), P + name_start, base_end - name_start);
    copy(static_cast<uint16_t*>(ext), P + ext_start, len - ext_start);
}
static PAPAYA_MS_ABI int hle_wtoi(const void* s) {
    const uint16_t* S = static_cast<const uint16_t*>(s);
    if (!S) return 0;
    long long v = 0; bool neg = false; size_t i = 0;
    while (S[i] == L' ' || S[i] == L'\t') ++i;
    if (S[i] == L'-') { neg = true; ++i; } else if (S[i] == L'+') ++i;
    while (S[i] >= L'0' && S[i] <= L'9') { v = v * 10 + (S[i] - L'0'); ++i; }
    return (int)(neg ? -v : v);
}
static PAPAYA_MS_ABI long hle_wtol(const void* s) {
    const uint16_t* S = static_cast<const uint16_t*>(s);
    if (!S) return 0;
    long long v = 0; bool neg = false; size_t i = 0;
    while (i < u16_len(S) && (S[i] == L' ' || S[i] == L'\t')) ++i;
    if (S[i] == L'-') { neg = true; ++i; } else if (S[i] == L'+') ++i;
    while (S[i] >= L'0' && S[i] <= L'9') { v = v * 10 + (S[i] - L'0'); ++i; }
    return (long)(neg ? -v : v);
}
static PAPAYA_MS_ABI unsigned long long hle_wcstoui64(const void* s, void** end, int base) {
    const uint16_t* S = static_cast<const uint16_t*>(s);
    if (!S) { if (end) *end = nullptr; return 0; }
    size_t i = 0; unsigned long long v = 0;
    while (S[i] == L' ' || S[i] == L'\t') ++i;
    bool neg = false; if (S[i] == L'-') { neg = true; ++i; } else if (S[i] == L'+') ++i;
    if (base == 0 || base == 16) { if (S[i] == L'0' && (S[i+1] == L'x' || S[i+1] == L'X')) { base = 16; i += 2; } else if (base == 0) base = 10; }
    if (base == 0) { if (S[i] == L'0') base = 8; else base = 10; }
    for (; S[i]; ++i) {
        unsigned d;
        if (S[i] >= L'0' && S[i] <= L'9') d = S[i] - L'0';
        else if (S[i] >= L'a' && S[i] <= L'f') d = S[i] - L'a' + 10;
        else if (S[i] >= L'A' && S[i] <= L'F') d = S[i] - L'A' + 10;
        else break;
        if ((unsigned)base <= d) break;
        v = v * (unsigned)base + d;
    }
    if (end) *end = const_cast<uint16_t*>(S + i);
    return neg ? (unsigned long long)-(long long)v : v;
}
static PAPAYA_MS_ABI void* hle_ui64tow(unsigned long long v, void* buf, int radix) {
    if (!buf) return nullptr;
    if (radix != 16 && radix != 8 && radix != 10) radix = 10;
    uint16_t tmp[65]; size_t n = 0;
    const char* hx = "0123456789abcdef";
    unsigned long long x = v;
    do { unsigned digit = (unsigned)(x % (unsigned)radix); tmp[n++] = (uint16_t)hx[digit]; x /= (unsigned)radix; } while (x);
    uint16_t* out = static_cast<uint16_t*>(buf);
    for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return out;
}
static PAPAYA_MS_ABI void* hle_wgetenv(const void* name) {
    const uint16_t* N = static_cast<const uint16_t*>(name);
    if (!N) return nullptr;
    char mb[1024]; size_t n = u16_len(N); if (n >= sizeof(mb)) n = sizeof(mb) - 1;
    for (size_t i = 0; i < n; ++i) mb[i] = (char)N[i];
    mb[n] = 0;
    const char* val = getenv(mb); if (!val) return nullptr;
    static uint16_t wv[2048]; size_t m = 0;
    for (; val[m] && m < 2047; ++m) wv[m] = (uint16_t)(unsigned char)val[m];
    wv[m] = 0;
    return wv;
}
static PAPAYA_MS_ABI void hle_wperror(const void* msg) {
    const uint16_t* M = static_cast<const uint16_t*>(msg);
    if (M && M[0]) { char mb[2048]; size_t n = u16_len(M); if (n >= sizeof(mb)) n = sizeof(mb) - 1; for (size_t i = 0; i < n; ++i) mb[i] = (char)M[i]; mb[n] = 0; fputs(mb, stderr); fputs(": ", stderr); }
    perror(nullptr);
}
static PAPAYA_MS_ABI time_t hle_time32(time_t* t) { time_t now = time(nullptr); if (t) *t = now; return now; }

// low-level fd I/O (used by UCRT _open/_read/_write/_lseek/_close/_chsize/_setmode)
static PAPAYA_MS_ABI int hle_open(const char* path, int oflag, int pmode) {
    (void)pmode;
    if (!path) return -1;
    std::string p = normalize_win_path(path);
    return ::open(p.c_str(), oflag);
}
static PAPAYA_MS_ABI int hle_wopen(const wchar_t* path, int oflag, int pmode) {
    if (!path) return -1;
    char mb[1024]; size_t n = wcstombs(mb, path, sizeof(mb) - 1); if (n == (size_t)-1) return -1; mb[n] = 0;
    std::string p = normalize_win_path(mb);
    return open(p.c_str(), oflag);
}
static PAPAYA_MS_ABI int hle_close(int fd) { return ::close(fd); }
static PAPAYA_MS_ABI int hle_read(int fd, void* buf, unsigned count) { return static_cast<int>(::read(fd, buf, count)); }
static PAPAYA_MS_ABI int hle_write(int fd, const void* buf, unsigned count) { return static_cast<int>(::write(fd, buf, count)); }
static PAPAYA_MS_ABI long long hle_lseek(int fd, long long off, int whence) { return ::lseek(fd, off, whence); }
static PAPAYA_MS_ABI int hle_chsize(int fd, long long size) { return ftruncate(fd, size) == 0 ? 0 : -1; }
static PAPAYA_MS_ABI int hle_fstat64i32(int fd, void* sb) { (void)fd; (void)sb; return 0; }
static PAPAYA_MS_ABI int hle_setmode(int fd, int mode) { (void)fd; (void)mode; return 0; }
static PAPAYA_MS_ABI int hle_kbhit() { return 0; }
static PAPAYA_MS_ABI int hle_putc(int c, FILE* stream) { return fputc(c, stream); }
static PAPAYA_MS_ABI int hle_fgetws(wchar_t* s, int n, void* stream) { return fgetws(s, n, static_cast<FILE*>(stream)) ? 0 : -1; }
static PAPAYA_MS_ABI int hle_fputws(const wchar_t* s, void* stream) { return fputws(s, static_cast<FILE*>(stream)) == EOF ? -1 : 0; }

static PAPAYA_MS_ABI int hle_vswprintf_helper(int* /*result*/, void* buf, size_t count, const wchar_t* fmt, void* args) {
    // Minimal: report truncation gracefully without formatting (safe no-op).
    (void)buf; (void)count; (void)fmt; (void)args;
    return -1;
}
static PAPAYA_MS_ABI int hle_stdio_common_vswscanf(void* opt, const wchar_t* str, const wchar_t* fmt, void* argptr) {
    (void)opt; (void)str; (void)fmt; (void)argptr;
    return 0;
}
static PAPAYA_MS_ABI int hle_pclose(void* pipe) { return pipe ? pclose(static_cast<FILE*>(pipe)) : -1; }
static PAPAYA_MS_ABI void* hle_wpopen(const wchar_t* cmd, const wchar_t* mode) {
    if (!cmd || !mode) return nullptr;
    char mcmd[2048], mmode[16];
    size_t nc = wcstombs(mcmd, cmd, sizeof(mcmd)-1); if (nc == (size_t)-1) return nullptr; mcmd[nc]=0;
    size_t nm = wcstombs(mmode, mode, sizeof(mmode)-1); if (nm == (size_t)-1) return nullptr; mmode[nm]=0;
    return popen(mcmd, mmode);
}

// stdio functions
static PAPAYA_MS_ABI void* hle_fopen(const char* filename, const char* mode) {
    if (!filename || !mode) return nullptr;
    std::string path = normalize_win_path(filename);
    FILE* f = fopen(path.c_str(), mode);
    register_host_file(f);
    return f;
}
static PAPAYA_MS_ABI void* hle_wfopen(const wchar_t* filename, const wchar_t* mode) {
    if (!filename || !mode) return nullptr;
    std::string mb_file = win_utf16_to_utf8(filename);
    std::string mb_mode = win_utf16_to_utf8(mode);
    return hle_fopen(mb_file.c_str(), mb_mode.c_str());
}
static PAPAYA_MS_ABI int hle_wfopen_s(void** pFile, const wchar_t* filename, const wchar_t* mode) {
    if (!pFile) return -1;
    *pFile = hle_wfopen(filename, mode);
    return *pFile ? 0 : -1;
}
static PAPAYA_MS_ABI void* hle_wfsopen(const wchar_t* filename, const wchar_t* mode, int shflag) {
    (void)shflag;
    return hle_wfopen(filename, mode);
}
static PAPAYA_MS_ABI int hle_fclose(void* stream) {
    FILE* f = host_file_for(stream);
    if (!f) return -1;
    unregister_host_file(f);
    return fclose(f);
}
static PAPAYA_MS_ABI size_t hle_fread(void* ptr, size_t size, size_t count, void* stream) {
    FILE* f = host_file_for(stream);
    return (ptr && f) ? fread(ptr, size, count, f) : 0;
}
static PAPAYA_MS_ABI int hle_fseek(void* stream, long offset, int origin) {
    FILE* f = host_file_for(stream);
    return f ? fseek(f, offset, origin) : -1;
}
static PAPAYA_MS_ABI long hle_ftell(void* stream) {
    FILE* f = host_file_for(stream);
    return f ? ftell(f) : -1;
}
static PAPAYA_MS_ABI int hle_fsetpos(void* stream, const fpos_t* pos) {
    FILE* f = host_file_for(stream);
    return (f && pos) ? fsetpos(f, pos) : -1;
}
static PAPAYA_MS_ABI int hle_fgetpos(void* stream, fpos_t* pos) {
    FILE* f = host_file_for(stream);
    return (f && pos) ? fgetpos(f, pos) : -1;
}
static PAPAYA_MS_ABI int hle_feof(void* stream) {
    FILE* f = host_file_for(stream);
    return f ? feof(f) : 1;
}
static PAPAYA_MS_ABI int hle_ferror(void* stream) {
    FILE* f = host_file_for(stream);
    return f ? ferror(f) : 1;
}
static PAPAYA_MS_ABI char* hle_fgets(char* str, int num, void* stream) {
    FILE* f = host_file_for(stream);
    return (str && f) ? fgets(str, num, f) : nullptr;
}
static PAPAYA_MS_ABI wint_t hle_fgetwc(void* stream) {
    FILE* f = host_file_for(stream);
    return f ? fgetwc(f) : WEOF;
}
static PAPAYA_MS_ABI wint_t hle_fputwc(wchar_t c, void* stream) {
    FILE* f = host_file_for(stream);
    return f ? fputwc(c, f) : WEOF;
}
static PAPAYA_MS_ABI int hle_getc(void* stream) {
    FILE* f = host_file_for(stream);
    return f ? getc(f) : EOF;
}
static PAPAYA_MS_ABI int hle_putchar(int c) {
    return putchar(c);
}
static PAPAYA_MS_ABI void hle_rewind(void* stream) {
    FILE* f = host_file_for(stream);
    if (f) rewind(f);
}
static PAPAYA_MS_ABI void hle_setbuf(void* stream, char* buffer) {
    FILE* f = host_file_for(stream);
    if (f) setbuf(f, buffer);
}
static PAPAYA_MS_ABI int hle_setvbuf(void* stream, char* buffer, int mode, size_t size) {
    FILE* f = host_file_for(stream);
    return f ? setvbuf(f, buffer, mode, size) : -1;
}
static PAPAYA_MS_ABI int hle_ungetc(int c, void* stream) {
    FILE* f = host_file_for(stream);
    return f ? ungetc(c, f) : EOF;
}
static PAPAYA_MS_ABI wint_t hle_ungetwc(wint_t c, void* stream) {
    FILE* f = host_file_for(stream);
    return f ? ungetwc(c, f) : WEOF;
}
static PAPAYA_MS_ABI int hle_freopen_s(void** pFile, const char* filename, const char* mode, void* oldStream) {
    if (!pFile) return -1;
    *pFile = nullptr;
    FILE* old = host_file_for(oldStream);
    if (!old || !mode) return -1;
    std::string path = filename ? normalize_win_path(filename) : "";
    FILE* nf = freopen(path.c_str(), mode, old);
    if (nf) {
        if (nf != old) unregister_host_file(old);
        register_host_file(nf);
    } else {
        unregister_host_file(old);
    }
    *pFile = nf;
    return nf ? 0 : -1;
}

// Convert a Windows x64 va_list (single char* into the callee home area:
// [4 gp x 8][4 xmm x 16][stack args...]) into a SysV __va_list_tag so host
// libc formatting works. The two formats differ structurally; guests always
// hand us the Windows layout (ms_abi). gp beyond 4 and fp beyond 4 are
// mapped from the contiguous stack slots; deeper args are out of scope.
struct VaTag { unsigned gp, fp; void* over; void* rsa; };
static void win_va_to_sysv(va_list win, va_list out) {
    static thread_local unsigned long long slots[6 + 8];  // 6 gp + 8 fp slots
    auto* w = reinterpret_cast<const unsigned long long*>(win);
    slots[0] = w[0]; slots[1] = w[1]; slots[2] = w[2]; slots[3] = w[3];
    const char* stack = reinterpret_cast<const char*>(win) + 0x60;
    slots[4] = *reinterpret_cast<const unsigned long long*>(stack);
    slots[5] = *reinterpret_cast<const unsigned long long*>(stack + 8);
    auto* fp = reinterpret_cast<double*>(reinterpret_cast<char*>(slots) + 48);
    const char* x = reinterpret_cast<const char*>(win);
    fp[0] = *reinterpret_cast<const double*>(x + 0x20);
    fp[1] = *reinterpret_cast<const double*>(x + 0x30);
    fp[2] = *reinterpret_cast<const double*>(x + 0x40);
    fp[3] = *reinterpret_cast<const double*>(x + 0x50);
    VaTag tag{0, 48, nullptr, slots};
    std::memcpy(out, &tag, sizeof(tag));   // out decays to __va_list_tag*
}

static PAPAYA_MS_ABI int hle_stdio_common_vfprintf(uint64_t options, void* stream, const char* format, void* locale, va_list valist) {
    (void)options; (void)locale;
    va_list ap;
    win_va_to_sysv(valist, ap);
    return vfprintf(static_cast<FILE*>(stream), format, ap);
}
static PAPAYA_MS_ABI int hle_stdio_common_vsprintf(uint64_t options, char* buffer, size_t buffer_count, const char* format, void* locale, va_list valist) {
    (void)options; (void)locale;
    if (!buffer || buffer_count == 0) return -1;
    va_list ap;
    win_va_to_sysv(valist, ap);
    return vsnprintf(buffer, buffer_count, format, ap);
}
static PAPAYA_MS_ABI int hle_stdio_common_vsnprintf_s(uint64_t options, char* buffer, size_t buffer_count, size_t max_count, const char* format, void* locale, va_list valist) {
    (void)options; (void)locale; (void)max_count;
    if (!buffer || buffer_count == 0) return -1;
    va_list ap;
    win_va_to_sysv(valist, ap);
    return vsnprintf(buffer, buffer_count, format, ap);
}
static PAPAYA_MS_ABI int hle_stdio_common_vsprintf_s(uint64_t options, char* buffer, size_t buffer_count, const char* format, void* locale, va_list valist) {
    (void)options; (void)locale;
    if (!buffer || buffer_count == 0) return -1;
    va_list ap;
    win_va_to_sysv(valist, ap);
    return vsnprintf(buffer, buffer_count, format, ap);
}
static PAPAYA_MS_ABI int hle_stdio_common_vswprintf(uint64_t options, wchar_t* buffer, size_t buffer_count, const wchar_t* format, void* locale, va_list valist) {
    (void)options; (void)locale;
    if (!buffer || buffer_count == 0) return -1;
    return vswprintf(buffer, buffer_count, format, valist);
}
static PAPAYA_MS_ABI int hle_stdio_common_vfwprintf(uint64_t options, void* stream, const wchar_t* format, void* locale, va_list valist) {
    (void)options; (void)locale;
    return vfwprintf(static_cast<FILE*>(stream), format, valist);
}
static PAPAYA_MS_ABI int hle_stdio_common_vfscanf(uint64_t options, void* stream, const char* format, void* locale, va_list valist) {
    (void)options; (void)locale;
    return vfscanf(static_cast<FILE*>(stream), format, valist);
}
static PAPAYA_MS_ABI int hle_stdio_common_vsscanf(uint64_t options, const char* buffer, size_t buffer_count, const char* format, void* locale, va_list valist) {
    (void)options; (void)buffer_count; (void)locale;
    return vsscanf(buffer, format, valist);
}
static PAPAYA_MS_ABI void* hle_acrt_iob_func(unsigned idx) {
    if (idx == 0) return stdin;
    if (idx == 1) return stdout;
    if (idx == 2) return stderr;
    return nullptr;
}
static PAPAYA_MS_ABI int* hle_p__commode() { static int c = 0; return &c; }
static PAPAYA_MS_ABI int* hle_p__fmode() { static int f = 0; return &f; }
static PAPAYA_MS_ABI int hle_fileno(void* stream) {
    FILE* f = host_file_for(stream);
    return f ? fileno(f) : -1;
}
static PAPAYA_MS_ABI int64_t hle_fseeki64(void* stream, int64_t offset, int origin) {
    FILE* f = host_file_for(stream);
    return f ? fseeko(f, offset, origin) : -1;
}
static PAPAYA_MS_ABI int64_t hle_ftelli64(void* stream) {
    FILE* f = host_file_for(stream);
    return f ? ftello(f) : -1;
}
static PAPAYA_MS_ABI intptr_t hle_get_osfhandle(int fd) { return fd; }
static PAPAYA_MS_ABI int hle_getmaxstdio() { return 512; }
static PAPAYA_MS_ABI int hle_setmaxstdio(int newmax) { return newmax; }
static PAPAYA_MS_ABI int hle_chsize_s(int fd, int64_t size) { return ftruncate(fd, size); }

// Additional USER32 stubs
static PAPAYA_MS_ABI BOOL hle_set_prop_w(void* hWnd, const wchar_t* lpString, void* hData) { (void)hWnd; (void)lpString; (void)hData; return 1; }
static PAPAYA_MS_ABI BOOL hle_set_menu_item_info_w(void* hMenu, u32 item, BOOL fByPos, void* lpmii) { (void)hMenu; (void)item; (void)fByPos; (void)lpmii; return 1; }
static PAPAYA_MS_ABI BOOL hle_set_window_display_affinity(void* hWnd, u32 dwAffinity) { (void)hWnd; (void)dwAffinity; return 1; }
static PAPAYA_MS_ABI BOOL hle_track_popup_menu_ex(void* hMenu, u32 uFlags, int x, int y, void* hWnd, void* lptpm) { (void)hMenu; (void)uFlags; (void)x; (void)y; (void)hWnd; (void)lptpm; return 0; }
static PAPAYA_MS_ABI BOOL hle_unregister_class_a(const char* lpClassName, void* hInstance) { (void)lpClassName; (void)hInstance; return 1; }
static PAPAYA_MS_ABI BOOL hle_unregister_class_w(const wchar_t* lpClassName, void* hInstance) { (void)lpClassName; (void)hInstance; return 1; }
static PAPAYA_MS_ABI BOOL hle_unregister_device_notification(void* handle) { (void)handle; return 1; }

extern "C" int hle_msvc_setjmp(void* jmp_buf);
__asm__(
    ".global hle_msvc_setjmp\n"
    "hle_msvc_setjmp:\n"
    "movq (%rsp), %r10\n"
    "movq %rdx, 0x00(%rcx)\n"
    "movq %rbx, 0x08(%rcx)\n"
    "movq %rsp, 0x10(%rcx)\n"
    "addq $8, 0x10(%rcx)\n"
    "movq %rbp, 0x18(%rcx)\n"
    "movq %rsi, 0x20(%rcx)\n"
    "movq %rdi, 0x28(%rcx)\n"
    "movq %r12, 0x30(%rcx)\n"
    "movq %r13, 0x38(%rcx)\n"
    "movq %r14, 0x40(%rcx)\n"
    "movq %r15, 0x48(%rcx)\n"
    "movq %r10, 0x50(%rcx)\n"
    "movdqa %xmm6, 0x60(%rcx)\n"
    "movdqa %xmm7, 0x70(%rcx)\n"
    "movdqa %xmm8, 0x80(%rcx)\n"
    "movdqa %xmm9, 0x90(%rcx)\n"
    "movdqa %xmm10, 0xa0(%rcx)\n"
    "movdqa %xmm11, 0xb0(%rcx)\n"
    "movdqa %xmm12, 0xc0(%rcx)\n"
    "movdqa %xmm13, 0xd0(%rcx)\n"
    "movdqa %xmm14, 0xe0(%rcx)\n"
    "movdqa %xmm15, 0xf0(%rcx)\n"
    "xorl %eax, %eax\n"
    "ret\n"
);

// ADVAPI32 Registry stubs
static PAPAYA_MS_ABI s32 hle_reg_enum_key_ex_w(void* hKey, u32 dwIndex, wchar_t* lpName, u32* lpcchName, u32* lpReserved, wchar_t* lpClass, u32* lpcchClass, void* lpftLastWriteTime) {
    (void)hKey; (void)dwIndex; (void)lpName; (void)lpcchName; (void)lpReserved; (void)lpClass; (void)lpcchClass; (void)lpftLastWriteTime;
    return 259; // ERROR_NO_MORE_ITEMS
}
static PAPAYA_MS_ABI s32 hle_reg_open_key_w(void* hKey, const wchar_t* lpSubKey, void** phkResult) {
    (void)hKey; (void)lpSubKey;
    if (phkResult) *phkResult = reinterpret_cast<void*>(0x2000);
    return 0; // ERROR_SUCCESS
}
static PAPAYA_MS_ABI s32 hle_reg_query_info_key_w(void* hKey, wchar_t* lpClass, u32* lpcchClass, u32* lpReserved, u32* lpcSubKeys, u32* lpcbMaxSubKeyLen, u32* lpcbMaxClassLen, u32* lpcValues, u32* lpcbMaxValueNameLen, u32* lpcbMaxValueLen, u32* lpcbSecurityDescriptor, void* lpftLastWriteTime) {
    (void)hKey; (void)lpClass; (void)lpcchClass; (void)lpReserved; (void)lpftLastWriteTime;
    if (lpcSubKeys) *lpcSubKeys = 0;
    if (lpcbMaxSubKeyLen) *lpcbMaxSubKeyLen = 0;
    if (lpcbMaxClassLen) *lpcbMaxClassLen = 0;
    if (lpcValues) *lpcValues = 0;
    if (lpcbMaxValueNameLen) *lpcbMaxValueNameLen = 0;
    if (lpcbMaxValueLen) *lpcbMaxValueLen = 0;
    if (lpcbSecurityDescriptor) *lpcbSecurityDescriptor = 0;
    return 0; // ERROR_SUCCESS
}

// NTDLL
static PAPAYA_MS_ABI u32 hle_rtl_nt_status_to_dos_error(u32 nt_status) { return nt_status & 0xFFFF; }
static PAPAYA_MS_ABI u32 hle_nt_query_information_file(HANDLE FileHandle, void* IoStatusBlock, void* FileInformation, u32 Length, u32 FileInformationClass) {
    (void)FileHandle; (void)IoStatusBlock; (void)FileInformation; (void)Length; (void)FileInformationClass;
    return 0;
}
static PAPAYA_MS_ABI u32 hle_nt_write_file(HANDLE FileHandle, HANDLE Event, void* ApcRoutine, void* ApcContext, void* IoStatusBlock, void* Buffer, u32 Length, void* ByteOffset, void* Key) {
    (void)FileHandle; (void)Event; (void)ApcRoutine; (void)ApcContext; (void)IoStatusBlock; (void)Buffer; (void)Length; (void)ByteOffset; (void)Key;
    return 0;
}

// WSOCK32
static PAPAYA_MS_ABI int hle_wsa_fd_is_set(int fd, void* set) {
    return FD_ISSET(fd, static_cast<fd_set*>(set)) ? 1 : 0;
}
static PAPAYA_MS_ABI int hle_ioctlsocket(int s, long cmd, u_long* argp) {
    (void)s; (void)cmd; (void)argp;
    return 0;
}

static PAPAYA_MS_ABI void* hle_uia_get_reserved_not_supported_value() {
    static int dummy = 0;
    return &dummy;
}
static PAPAYA_MS_ABI int hle_uia_host_provider_from_hwnd(void* hwnd, void** ppProvider) {
    (void)hwnd;
    if (ppProvider) *ppProvider = nullptr;
    return 0; // S_OK
}
static PAPAYA_MS_ABI int hle_uia_lookup_id(int type, const void* pGuid) {
    (void)type; (void)pGuid;
    return 1;
}
static PAPAYA_MS_ABI int hle_uia_raise_automation_event(void* pProvider, int id) {
    (void)pProvider; (void)id;
    return 0; // S_OK
}
static PAPAYA_MS_ABI int hle_uia_raise_automation_property_changed_event(void* pProvider, int id, void* oldValue, void* newValue) {
    (void)pProvider; (void)id; (void)oldValue; (void)newValue;
    return 0; // S_OK
}
static PAPAYA_MS_ABI int64_t hle_uia_return_raw_element_provider(void* hwnd, uint64_t wParam, int64_t lParam, void* el) {
    (void)hwnd; (void)wParam; (void)lParam; (void)el;
    return 0;
}

static PAPAYA_MS_ABI void* hle_rtl_pc_to_file_header(void* PcValue, void** BaseOfImage) {
    (void)PcValue;
    u64 base = seh_image_base();
    if (BaseOfImage) *BaseOfImage = reinterpret_cast<void*>(base);
    return reinterpret_cast<void*>(base);
}

// -------------------------------------------------------------
// Extended USER32 Functions
// -------------------------------------------------------------
struct Win32MonitorInfoW {
    u32 cbSize;
    struct { s32 left, top, right, bottom; } rcMonitor;
    struct { s32 left, top, right, bottom; } rcWork;
    u32 dwFlags;
    char16_t szDevice[32];
};

static PAPAYA_MS_ABI BOOL hle_get_monitor_info_w(void* hMonitor, void* lpmi) {
    (void)hMonitor;
    if (!lpmi) return FALSE_VAL;
    auto* mi = static_cast<Win32MonitorInfoW*>(lpmi);
    mi->rcMonitor = { 0, 0, 1920, 1080 };
    mi->rcWork    = { 0, 0, 1920, 1080 };
    mi->dwFlags   = 1; // MONITORINFOF_PRIMARY
    win_copy_u16(mi->szDevice, u"\\\\.\\DISPLAY1", 32);
    return TRUE_VAL;
}

struct Win32DevModeW {
    char16_t dmDeviceName[32];
    u16 dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    u32 dmFields;
    s32 dmPositionX, dmPositionY;
    u32 dmDisplayOrientation, dmDisplayFixedOutput;
    s16 dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    char16_t dmFormName[32];
    u16 dmLogPixels;
    u32 dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
};

static PAPAYA_MS_ABI BOOL hle_enum_display_settings_w(const void* lpszDeviceName, u32 iModeNum, void* lpDevMode) {
    (void)lpszDeviceName;
    if (!lpDevMode || iModeNum > 0) return FALSE_VAL;
    auto* dm = static_cast<Win32DevModeW*>(lpDevMode);
    dm->dmPelsWidth = 1920;
    dm->dmPelsHeight = 1080;
    dm->dmBitsPerPel = 32;
    dm->dmDisplayFrequency = 60;
    dm->dmFields = 0x00040000 | 0x00080000 | 0x00100000 | 0x00400000;
    win_copy_u16(dm->dmDeviceName, u"\\\\.\\DISPLAY1", 32);
    win_copy_u16(dm->dmFormName, u"", 32);
    return TRUE_VAL;
}

static PAPAYA_MS_ABI BOOL hle_is_window(void* hWnd) { return hWnd ? TRUE_VAL : FALSE_VAL; }
static PAPAYA_MS_ABI BOOL hle_is_window_visible(void* hWnd) { return hWnd ? TRUE_VAL : FALSE_VAL; }
static PAPAYA_MS_ABI BOOL hle_is_zoomed(void* hWnd) { (void)hWnd; return FALSE_VAL; }
static PAPAYA_MS_ABI BOOL hle_is_iconic(void* hWnd) { (void)hWnd; return FALSE_VAL; }
static PAPAYA_MS_ABI BOOL hle_move_window(void* hWnd, int x, int y, int w, int h, BOOL bRepaint) {
    (void)bRepaint;
    return Win32ApiHle::hle_set_window_pos(hWnd, nullptr, x, y, w, h, 0x0040) ? TRUE_VAL : FALSE_VAL;
}
static PAPAYA_MS_ABI BOOL hle_flash_window_ex(void* pfwi) { (void)pfwi; return TRUE_VAL; }
static PAPAYA_MS_ABI void* hle_set_focus(void* hWnd) { return hWnd; }
static PAPAYA_MS_ABI BOOL hle_allow_set_foreground_window(u32 dwProcessId) { (void)dwProcessId; return TRUE_VAL; }
static PAPAYA_MS_ABI u64 hle_get_message_extra_info() { return 0; }
static PAPAYA_MS_ABI BOOL hle_track_mouse_event(void* lpEventTrack) { (void)lpEventTrack; return TRUE_VAL; }
static PAPAYA_MS_ABI s64 hle_call_window_proc_w(void* lpPrevWndFunc, void* hWnd, u32 Msg, u64 wParam, s64 lParam) {
    if (!lpPrevWndFunc) return 0;
    auto proc = reinterpret_cast<s64 (PAPAYA_MS_ABI *)(void*, u32, u64, s64)>(lpPrevWndFunc);
    return proc(hWnd, Msg, wParam, lParam);
}

static PAPAYA_MS_ABI u32 hle_get_raw_input_device_list(void* pRawInputDeviceList, u32* puiNumDevices, u32 cbSize) {
    (void)pRawInputDeviceList; (void)cbSize;
    if (puiNumDevices) *puiNumDevices = 0;
    return 0;
}
static PAPAYA_MS_ABI u32 hle_get_raw_input_device_info_a(void* hDevice, u32 uiCommand, void* pData, u32* pcbSize) {
    (void)hDevice; (void)uiCommand; (void)pData;
    if (pcbSize) *pcbSize = 0;
    return static_cast<u32>(-1);
}
static PAPAYA_MS_ABI BOOL hle_register_raw_input_devices(void* pRawInputDevices, u32 uiNumDevices, u32 cbSize) {
    (void)pRawInputDevices; (void)uiNumDevices; (void)cbSize;
    return TRUE_VAL;
}
static PAPAYA_MS_ABI u32 hle_get_raw_input_data(void* hRawInput, u32 uiCommand, void* pData, u32* pcbSize, u32 cbSizeHeader) {
    (void)hRawInput; (void)uiCommand; (void)pData; (void)cbSizeHeader;
    if (pcbSize) *pcbSize = 0;
    return static_cast<u32>(-1);
}
static PAPAYA_MS_ABI void* hle_create_icon_indirect(void* piconinfo) { (void)piconinfo; return reinterpret_cast<void*>(0x1C001); }
static PAPAYA_MS_ABI void* hle_create_icon_from_resource(void* presbits, u32 dwResSize, BOOL fIcon, u32 dwVer) { (void)presbits; (void)dwResSize; (void)fIcon; (void)dwVer; return reinterpret_cast<void*>(0x1C002); }
static PAPAYA_MS_ABI BOOL hle_destroy_icon(void* hIcon) { (void)hIcon; return TRUE_VAL; }
static PAPAYA_MS_ABI void* hle_set_windows_hook_ex_a(int idHook, void* lpfn, void* hmod, u32 dwThreadId) { (void)idHook; (void)lpfn; (void)hmod; (void)dwThreadId; return reinterpret_cast<void*>(0x1001); }
static PAPAYA_MS_ABI BOOL hle_unhook_windows_hook_ex(void* hhk) { (void)hhk; return TRUE_VAL; }
static PAPAYA_MS_ABI s64 hle_call_next_hook_ex(void* hhk, int nCode, u64 wParam, s64 lParam) { (void)hhk; (void)nCode; (void)wParam; (void)lParam; return 0; }
static PAPAYA_MS_ABI BOOL hle_clip_cursor(const void* lpRect) { (void)lpRect; return TRUE_VAL; }
static PAPAYA_MS_ABI void* hle_window_from_point(s32 x, s32 y) { (void)x; (void)y; return nullptr; }
static PAPAYA_MS_ABI BOOL hle_create_caret(void* hWnd, void* hBitmap, int nWidth, int nHeight) { (void)hWnd; (void)hBitmap; (void)nWidth; (void)nHeight; return TRUE_VAL; }
static PAPAYA_MS_ABI BOOL hle_destroy_caret() { return TRUE_VAL; }
static PAPAYA_MS_ABI BOOL hle_set_caret_pos(int X, int Y) { (void)X; (void)Y; return TRUE_VAL; }

static PAPAYA_MS_ABI BOOL hle_open_clipboard(void* hWndNewOwner) { (void)hWndNewOwner; return TRUE_VAL; }
static PAPAYA_MS_ABI BOOL hle_close_clipboard() { return TRUE_VAL; }
static PAPAYA_MS_ABI BOOL hle_empty_clipboard() { return TRUE_VAL; }
static PAPAYA_MS_ABI void* hle_get_clipboard_data(u32 uFormat) { (void)uFormat; return nullptr; }
static PAPAYA_MS_ABI void* hle_set_clipboard_data(u32 uFormat, void* hMem) { (void)uFormat; return hMem; }
static PAPAYA_MS_ABI BOOL hle_is_clipboard_format_available(u32 format) { (void)format; return FALSE_VAL; }

static PAPAYA_MS_ABI int hle_to_unicode_ex(u32 wVirtKey, u32 wScanCode, const u8* lpKeyState, wchar_t* pwszBuff, int cchBuff, u32 wFlags, void* dwhkl) {
    (void)wScanCode; (void)lpKeyState; (void)wFlags; (void)dwhkl;
    if (!pwszBuff || cchBuff <= 0) return 0;
    if (wVirtKey >= 32 && wVirtKey < 127) { pwszBuff[0] = static_cast<wchar_t>(wVirtKey); return 1; }
    return 0;
}
static PAPAYA_MS_ABI u32 hle_map_virtual_key_ex_a(u32 uCode, u32 uMapType, void* dwhkl) { (void)uMapType; (void)dwhkl; return uCode; }
static PAPAYA_MS_ABI void* hle_get_keyboard_layout(u32 idThread) { (void)idThread; return reinterpret_cast<void*>(0x04090409); }
static PAPAYA_MS_ABI int hle_get_keyboard_layout_list(int nBuff, void** lpList) {
    if (nBuff > 0 && lpList) lpList[0] = reinterpret_cast<void*>(0x04090409);
    return 1;
}
static PAPAYA_MS_ABI void* hle_activate_keyboard_layout(void* hkl, u32 Flags) { (void)Flags; return hkl; }
static PAPAYA_MS_ABI BOOL hle_get_update_rect(void* hWnd, void* lpRect, BOOL bErase) { (void)hWnd; (void)lpRect; (void)bErase; return FALSE_VAL; }
static PAPAYA_MS_ABI int hle_set_window_rgn(void* hWnd, void* hRgn, BOOL bRedraw) { (void)hWnd; (void)hRgn; (void)bRedraw; return 1; }
static PAPAYA_MS_ABI BOOL hle_register_touch_window(void* hWnd, u32 ulFlags) { (void)hWnd; (void)ulFlags; return TRUE_VAL; }
static PAPAYA_MS_ABI BOOL hle_close_touch_input_handle(void* hTouchInput) { (void)hTouchInput; return TRUE_VAL; }
static PAPAYA_MS_ABI BOOL hle_get_touch_input_info(void* hTouchInput, u32 cInputs, void* pInputs, int cbSize) { (void)hTouchInput; (void)cInputs; (void)pInputs; (void)cbSize; return FALSE_VAL; }

// -------------------------------------------------------------
// Extended GDI32 / KERNEL32 / OLE / WINMM / IPHLPAPI Functions
// -------------------------------------------------------------
static PAPAYA_MS_ABI void* hle_create_rect_rgn(int x1, int y1, int x2, int y2) { (void)x1; (void)y1; (void)x2; (void)y2; return reinterpret_cast<void*>(0x2001); }
static PAPAYA_MS_ABI void* hle_create_polygon_rgn(const void* pptl, int cPoint, int iMode) { (void)pptl; (void)cPoint; (void)iMode; return reinterpret_cast<void*>(0x2002); }
static PAPAYA_MS_ABI void* hle_create_bitmap(int nWidth, int nHeight, u32 nPlanes, u32 nBitCount, const void* lpBits) {
    (void)nPlanes; (void)nBitCount; (void)lpBits;
    return std::malloc(nWidth * nHeight * 4);
}
static PAPAYA_MS_ABI void* hle_create_dib_section(void* hdc, const void* pbmi, u32 usage, void** ppvBits, void* hSection, u32 offset) {
    (void)hdc; (void)pbmi; (void)usage; (void)hSection; (void)offset;
    if (ppvBits) *ppvBits = std::malloc(1920 * 1080 * 4);
    return reinterpret_cast<void*>(0x3001);
}
static PAPAYA_MS_ABI BOOL hle_path_file_exists_w(const wchar_t* pszPath) {
    if (!pszPath) return FALSE_VAL;
    std::string u8 = win_utf16_to_utf8(pszPath);
    return std::filesystem::exists(u8) ? TRUE_VAL : FALSE_VAL;
}
static PAPAYA_MS_ABI int hle_lcid_to_locale_name(u32 Locale, void* lpName, int cchName, u32 dwFlags) {
    (void)Locale; (void)dwFlags;
    if (!lpName || cchName <= 0) return 0;
    win_copy_u16(lpName, u"en-US", static_cast<size_t>(cchName));
    return 6;
}
static PAPAYA_MS_ABI size_t hle_heap_size(void* hHeap, u32 dwFlags, const void* lpMem) {
    (void)hHeap; (void)dwFlags;
    if (!lpMem) return 0;
    const auto* hdr = reinterpret_cast<const Win32HeapHeader*>(lpMem) - 1;
    if (hdr->magic == 0x50415059) return hdr->size;
    return 0;
}
static PAPAYA_MS_ABI BOOL hle_k32_get_performance_info(void* pPerformanceInformation, u32 cb) {
    (void)cb;
    if (!pPerformanceInformation) return FALSE_VAL;
    std::memset(pPerformanceInformation, 0, cb);
    return TRUE_VAL;
}
static PAPAYA_MS_ABI BOOL hle_terminate_process(void* hProcess, u32 uExitCode) {
    (void)hProcess;
    _exit(static_cast<int>(uExitCode));
}
static PAPAYA_MS_ABI size_t hle_get_large_page_minimum() { return 2 * 1024 * 1024; }
static PAPAYA_MS_ABI BOOL hle_attach_console(u32 dwProcessId) { (void)dwProcessId; return TRUE_VAL; }
static PAPAYA_MS_ABI BOOL hle_get_console_mode(void* hConsoleHandle, u32* lpMode) { (void)hConsoleHandle; if (lpMode) *lpMode = 7; return TRUE_VAL; }
static PAPAYA_MS_ABI BOOL hle_set_console_mode(void* hConsoleHandle, u32 dwMode) { (void)hConsoleHandle; (void)dwMode; return TRUE_VAL; }
static PAPAYA_MS_ABI u32 hle_get_console_output_cp() { return 65001; } // UTF-8
static PAPAYA_MS_ABI BOOL hle_get_console_screen_buffer_info(void* hConsoleOutput, void* lpConsoleScreenBufferInfo) {
    (void)hConsoleOutput;
    if (lpConsoleScreenBufferInfo) std::memset(lpConsoleScreenBufferInfo, 0, 22);
    return TRUE_VAL;
}
static PAPAYA_MS_ABI BOOL hle_set_console_text_attribute(void* hConsoleOutput, u16 wAttributes) { (void)hConsoleOutput; (void)wAttributes; return TRUE_VAL; }
static PAPAYA_MS_ABI BOOL hle_write_console_w(void* hConsoleOutput, const void* lpBuffer, u32 nNumberOfCharsToWrite, u32* lpNumberOfCharsWritten, void* lpReserved) {
    (void)hConsoleOutput; (void)lpReserved;
    if (lpBuffer && nNumberOfCharsToWrite > 0) {
        std::string u8 = win_utf16_to_utf8(lpBuffer, nNumberOfCharsToWrite);
        std::fwrite(u8.data(), 1, u8.size(), stdout);
        std::fflush(stdout);
    }
    if (lpNumberOfCharsWritten) *lpNumberOfCharsWritten = nNumberOfCharsToWrite;
    return TRUE_VAL;
}
static PAPAYA_MS_ABI BOOL hle_read_console_w(void* hConsoleInput, void* lpBuffer, u32 nNumberOfCharsToRead, u32* lpNumberOfCharsRead, void* pInputControl) {
    (void)hConsoleInput; (void)lpBuffer; (void)nNumberOfCharsToRead; (void)pInputControl;
    if (lpNumberOfCharsRead) *lpNumberOfCharsRead = 0;
    return TRUE_VAL;
}
static PAPAYA_MS_ABI void hle_prop_variant_clear(void* pvar) {
    if (pvar) std::memset(pvar, 0, 24);
}
static PAPAYA_MS_ABI void* hle_sys_alloc_string(const void* psz) {
    if (!psz) return nullptr;
    const auto* p = static_cast<const char16_t*>(psz);
    size_t len = 0;
    while (p[len]) len++;
    u32 byte_len = static_cast<u32>(len * sizeof(char16_t));
    u8* mem = static_cast<u8*>(std::malloc(4 + byte_len + sizeof(char16_t)));
    *reinterpret_cast<u32*>(mem) = byte_len;
    char16_t* str = reinterpret_cast<char16_t*>(mem + 4);
    std::memcpy(str, p, byte_len + sizeof(char16_t));
    return str;
}
static PAPAYA_MS_ABI void* hle_sys_alloc_string_len(const void* strIn, u32 ui) {
    u32 byte_len = ui * sizeof(char16_t);
    u8* mem = static_cast<u8*>(std::malloc(4 + byte_len + sizeof(char16_t)));
    *reinterpret_cast<u32*>(mem) = byte_len;
    char16_t* str = reinterpret_cast<char16_t*>(mem + 4);
    if (strIn) std::memcpy(str, strIn, byte_len);
    str[ui] = 0;
    return str;
}
static PAPAYA_MS_ABI void hle_sys_free_string(void* bstr) {
    if (bstr) std::free(static_cast<u8*>(bstr) - 4);
}
static PAPAYA_MS_ABI u32 hle_sys_string_len(void* bstr) {
    if (!bstr) return 0;
    return *reinterpret_cast<u32*>(static_cast<u8*>(bstr) - 4) / sizeof(char16_t);
}

static PAPAYA_MS_ABI u32 hle_midi_in_get_num_devs() { return 0; }
static PAPAYA_MS_ABI u32 hle_midi_in_get_dev_caps_a(u64 uDeviceID, void* pmcic, u32 cbpmcic) { (void)uDeviceID; (void)pmcic; (void)cbpmcic; return 2; }
static PAPAYA_MS_ABI u32 hle_midi_in_open(void** lphMidiIn, u32 uDeviceID, u64 dwCallback, u64 dwInstance, u32 fdwOpen) {
    (void)lphMidiIn; (void)uDeviceID; (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    return 2;
}
static PAPAYA_MS_ABI u32 hle_midi_in_close(void* hMidiIn) { (void)hMidiIn; return 0; }
static PAPAYA_MS_ABI u32 hle_midi_in_start(void* hMidiIn) { (void)hMidiIn; return 0; }
static PAPAYA_MS_ABI u32 hle_midi_in_stop(void* hMidiIn) { (void)hMidiIn; return 0; }
static PAPAYA_MS_ABI u32 hle_midi_in_get_id(void* hMidiIn, u32* puDeviceID) { (void)hMidiIn; if (puDeviceID) *puDeviceID = 0; return 0; }
static PAPAYA_MS_ABI u32 hle_midi_in_get_error_text_a(u32 mmrError, char* pszText, u32 cchText) {
    (void)mmrError;
    if (pszText && cchText > 0) pszText[0] = '\0';
    return 0;
}
static PAPAYA_MS_ABI u32 hle_get_adapters_addresses(u32 Family, u32 Flags, void* Reserved, void* AdapterAddresses, u32* SizePointer) {
    (void)Family; (void)Flags; (void)Reserved; (void)AdapterAddresses;
    if (SizePointer) *SizePointer = 0;
    return 232;
}
static PAPAYA_MS_ABI u32 hle_get_best_interface_ex(void* pDestAddr, u32* pdwBestIfIndex) {
    (void)pDestAddr;
    if (pdwBestIfIndex) *pdwBestIfIndex = 1;
    return 0;
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


// Codegen attribution stubs (generated by scripts/gen_thunks.py): fills the
// remaining safe game-facing export surface so imports resolve. Real impls win.
#include "gen_codegen_stubs.inc"
// Host-libc-backed CRT math forwards (real implementations, generated).
#include "gen_crt_math.inc"

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

    // Memory-mapped files
    register_function("KERNEL32.DLL", "CreateFileMappingA",       reinterpret_cast<void*>(&hle_create_file_mapping_a));
    register_function("KERNEL32.DLL", "CreateFileMappingW",       reinterpret_cast<void*>(&hle_create_file_mapping_w));
    register_function("KERNEL32.DLL", "MapViewOfFile",            reinterpret_cast<void*>(&hle_map_view_of_file));
    register_function("KERNEL32.DLL", "MapViewOfFileEx",          reinterpret_cast<void*>(&hle_map_view_of_file_ex));
    register_function("KERNEL32.DLL", "UnmapViewOfFile",          reinterpret_cast<void*>(&hle_unmap_view_of_file));
    // String + misc
    register_function("KERNEL32.DLL", "lstrcmpW",                 reinterpret_cast<void*>(&hle_lstrcmp_w));
    register_function("KERNEL32.DLL", "lstrcmpiA",                reinterpret_cast<void*>(&hle_lstrcmpi_a));
    register_function("KERNEL32.DLL", "lstrcmpiW",                reinterpret_cast<void*>(&hle_lstrcmpi_w));
    register_function("KERNEL32.DLL", "MulDiv",                   reinterpret_cast<void*>(&hle_mul_div));
    register_function("KERNEL32.DLL", "IsWow64Process",           reinterpret_cast<void*>(&hle_is_wow64_process));
    register_function("KERNEL32.DLL", "IsBadStringPtrA",          reinterpret_cast<void*>(&hle_is_bad_string_ptr_a));
    register_function("KERNEL32.DLL", "IsBadStringPtrW",          reinterpret_cast<void*>(&hle_is_bad_string_ptr_w));
    register_function("KERNEL32.DLL", "GetTempPathW",             reinterpret_cast<void*>(&hle_get_temp_path_w));
    register_function("KERNEL32.DLL", "GetSystemDirectoryW",      reinterpret_cast<void*>(&hle_get_system_directory_w));

    register_function("KERNEL32.DLL", "TlsAlloc", reinterpret_cast<void*>(&hle_tls_alloc));
    register_function("KERNEL32.DLL", "TlsFree", reinterpret_cast<void*>(&hle_tls_free));
    register_function("KERNEL32.DLL", "TlsGetValue", reinterpret_cast<void*>(&hle_tls_get_value));
    register_function("KERNEL32.DLL", "TlsSetValue", reinterpret_cast<void*>(&hle_tls_set_value));

    register_function("KERNEL32.DLL", "FlsAlloc", reinterpret_cast<void*>(&hle_fls_alloc));
    register_function("KERNEL32.DLL", "FlsFree", reinterpret_cast<void*>(&hle_fls_free));
    register_function("KERNEL32.DLL", "FlsGetValue", reinterpret_cast<void*>(&hle_fls_get_value));
    register_function("KERNEL32.DLL", "FlsSetValue", reinterpret_cast<void*>(&hle_fls_set_value));

    register_function("api-ms-win-core-fibers-l1-1-0.dll", "FlsAlloc", reinterpret_cast<void*>(&hle_fls_alloc));
    register_function("api-ms-win-core-fibers-l1-1-0.dll", "FlsFree", reinterpret_cast<void*>(&hle_fls_free));
    register_function("api-ms-win-core-fibers-l1-1-0.dll", "FlsGetValue", reinterpret_cast<void*>(&hle_fls_get_value));
    register_function("api-ms-win-core-fibers-l1-1-0.dll", "FlsSetValue", reinterpret_cast<void*>(&hle_fls_set_value));

    register_function("api-ms-win-core-fibers-l1-1-1.dll", "FlsAlloc", reinterpret_cast<void*>(&hle_fls_alloc));
    register_function("api-ms-win-core-fibers-l1-1-1.dll", "FlsFree", reinterpret_cast<void*>(&hle_fls_free));
    register_function("api-ms-win-core-fibers-l1-1-1.dll", "FlsGetValue", reinterpret_cast<void*>(&hle_fls_get_value));
    register_function("api-ms-win-core-fibers-l1-1-1.dll", "FlsSetValue", reinterpret_cast<void*>(&hle_fls_set_value));

    register_function("api-ms-win-core-util-l1-1-0.dll", "EncodePointer", reinterpret_cast<void*>(&hle_encode_pointer));
    register_function("api-ms-win-core-util-l1-1-0.dll", "DecodePointer", reinterpret_cast<void*>(&hle_decode_pointer));
    register_function("api-ms-win-core-util-l1-1-0.dll", "EncodeSystemPointer", reinterpret_cast<void*>(&hle_encode_system_pointer));
    register_function("api-ms-win-core-util-l1-1-0.dll", "DecodeSystemPointer", reinterpret_cast<void*>(&hle_decode_system_pointer));

    register_function("api-ms-win-core-util-l1-1-1.dll", "EncodePointer", reinterpret_cast<void*>(&hle_encode_pointer));
    register_function("api-ms-win-core-util-l1-1-1.dll", "DecodePointer", reinterpret_cast<void*>(&hle_decode_pointer));

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
    register_function("KERNEL32.DLL", "InitializeCriticalSectionEx", reinterpret_cast<void*>(&hle_init_critical_section_ex));
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

    register_function("KERNEL32.DLL", "ExpandEnvironmentStringsA", reinterpret_cast<void*>(&hle_expand_environment_strings_a));
    register_function("KERNEL32.DLL", "ExpandEnvironmentStringsW", reinterpret_cast<void*>(&hle_expand_environment_strings_w));
    register_function("KERNEL32.DLL", "GetDiskFreeSpaceExW", reinterpret_cast<void*>(&hle_get_disk_free_space_ex_w));
    register_function("KERNEL32.DLL", "SetFileAttributesA", reinterpret_cast<void*>(&hle_set_file_attributes_a));
    register_function("KERNEL32.DLL", "SetFileAttributesW", reinterpret_cast<void*>(&hle_set_file_attributes_w));
    register_function("KERNEL32.DLL", "SetFileTime", reinterpret_cast<void*>(&hle_set_file_time));
    register_function("KERNEL32.DLL", "SuspendThread", reinterpret_cast<void*>(&hle_suspend_thread));
    register_function("KERNEL32.DLL", "ResumeThread", reinterpret_cast<void*>(&hle_resume_thread));
    register_function("KERNEL32.DLL", "CreateToolhelp32Snapshot", reinterpret_cast<void*>(&hle_create_toolhelp32_snapshot));
    register_function("KERNEL32.DLL", "Thread32First", reinterpret_cast<void*>(&hle_thread32_first));
    register_function("KERNEL32.DLL", "Thread32Next", reinterpret_cast<void*>(&hle_thread32_next));
    register_function("OLE32.dll", "OleInitialize", reinterpret_cast<void*>(&hle_ole_initialize));
    register_function("OLE32.dll", "OleUninitialize", reinterpret_cast<void*>(&hle_ole_uninitialize));
    register_function("OLE32.dll", "RegisterDragDrop", reinterpret_cast<void*>(&hle_register_drag_drop));
    register_function("OLE32.dll", "RevokeDragDrop", reinterpret_cast<void*>(&hle_revoke_drag_drop));
    register_function("OLE32.dll", "SetErrorInfo", reinterpret_cast<void*>(&hle_set_error_info));
    register_function("OLE32.dll", "GetErrorInfo", reinterpret_cast<void*>(&hle_get_error_info));
    register_function("OLE32.dll", "CreateErrorInfo", reinterpret_cast<void*>(&hle_create_error_info));
    register_function("OLE32.dll", "CoCreateFreeThreadedMarshaler", reinterpret_cast<void*>(&hle_co_create_free_threaded_marshaler));
    register_function("OLE32.dll", "ReleaseStgMedium", reinterpret_cast<void*>(&hle_release_stg_medium));
    register_function("KERNEL32.DLL", "CreateFileA", reinterpret_cast<void*>(&hle_create_file_a));
    register_function("KERNEL32.DLL", "CreateFileW", reinterpret_cast<void*>(&hle_create_file_w));
    register_function("KERNEL32.DLL", "ReadFile", reinterpret_cast<void*>(&hle_read_file));
    register_function("KERNEL32.DLL", "WriteFile", reinterpret_cast<void*>(&hle_write_file));
    register_function("KERNEL32.DLL", "CloseHandle", reinterpret_cast<void*>(&hle_close_handle));
    register_function("KERNEL32.DLL", "GetFileSize", reinterpret_cast<void*>(&hle_get_file_size));
    register_function("KERNEL32.DLL", "SetFilePointer", reinterpret_cast<void*>(&hle_set_file_pointer));
    register_function("KERNEL32.DLL", "SetFilePointerEx", reinterpret_cast<void*>(&hle_set_file_pointer_ex));
    register_function("KERNEL32.DLL", "GetFileSizeEx", reinterpret_cast<void*>(&hle_get_file_size_ex));
    register_function("KERNEL32.DLL", "GetFileAttributesA", reinterpret_cast<void*>(&hle_get_file_attributes_a));
    register_function("KERNEL32.DLL", "GetFileAttributesW", reinterpret_cast<void*>(&hle_get_file_attributes_w));
    register_function("KERNEL32.DLL", "GetFileAttributesExA", reinterpret_cast<void*>(&hle_get_file_attributes_ex_a));
    register_function("KERNEL32.DLL", "GetFileAttributesExW", reinterpret_cast<void*>(&hle_get_file_attributes_ex_w));
    register_function("KERNEL32.DLL", "GetFullPathNameA", reinterpret_cast<void*>(&hle_get_full_path_name_a));
    register_function("KERNEL32.DLL", "GetFullPathNameW", reinterpret_cast<void*>(&hle_get_full_path_name_w));
    register_function("KERNEL32.DLL", "GetCurrentDirectoryA", reinterpret_cast<void*>(&hle_get_current_directory_a));
    register_function("KERNEL32.DLL", "GetCurrentDirectoryW", reinterpret_cast<void*>(&hle_get_current_directory_w));
    register_function("KERNEL32.DLL", "SetCurrentDirectoryA", reinterpret_cast<void*>(&hle_set_current_directory_a));
    register_function("KERNEL32.DLL", "SetCurrentDirectoryW", reinterpret_cast<void*>(&hle_set_current_directory_w));
    register_function("KERNEL32.DLL", "CreateDirectoryW", reinterpret_cast<void*>(&hle_create_directory_w));
    register_function("KERNEL32.DLL", "DeleteFileW", reinterpret_cast<void*>(&hle_delete_file_w));
    register_function("KERNEL32.DLL", "GetFileInformationByHandle", reinterpret_cast<void*>(&hle_get_file_information_by_handle));
    register_function("KERNEL32.DLL", "GetFileType", reinterpret_cast<void*>(&hle_get_file_type));
    register_function("KERNEL32.DLL", "FormatMessageW", reinterpret_cast<void*>(&hle_format_message_w));
    register_function("KERNEL32.DLL", "GetUserDefaultUILanguage", reinterpret_cast<void*>(&hle_get_user_default_ui_language));
    register_function("KERNEL32.DLL", "GetUserDefaultLCID", reinterpret_cast<void*>(&hle_get_user_default_lcid));
    register_function("KERNEL32.DLL", "GetLocaleInfoW", reinterpret_cast<void*>(&hle_get_locale_info_w));
    register_function("KERNEL32.DLL", "GetLocaleInfoEx", reinterpret_cast<void*>(&hle_get_locale_info_ex));
    register_function("KERNEL32.DLL", "LCMapStringW", reinterpret_cast<void*>(&hle_lc_map_string_w));
    register_function("KERNEL32.DLL", "LCMapStringEx", reinterpret_cast<void*>(&hle_lc_map_string_w));
    register_function("KERNEL32.DLL", "CompareStringW", reinterpret_cast<void*>(&hle_compare_string_w));
    register_function("KERNEL32.DLL", "CompareStringOrdinal", reinterpret_cast<void*>(&hle_compare_string_ordinal));
    register_function("KERNEL32.DLL", "CompareFileTime", reinterpret_cast<void*>(&hle_compare_file_time));
    register_function("KERNEL32.DLL", "SystemTimeToTzSpecificLocalTime", reinterpret_cast<void*>(&hle_system_time_to_tz_specific_local_time));
    register_function("KERNEL32.DLL", "FileTimeToSystemTime", reinterpret_cast<void*>(&hle_file_time_to_system_time));
    register_function("KERNEL32.DLL", "SystemTimeToFileTime", reinterpret_cast<void*>(&hle_system_time_to_file_time));
    register_function("KERNEL32.DLL", "GetTimeZoneInformation", reinterpret_cast<void*>(&hle_get_time_zone_information));
    register_function("KERNEL32.DLL", "GetVolumeInformationW", reinterpret_cast<void*>(&hle_get_volume_information_w));
    register_function("KERNEL32.DLL", "GetDiskFreeSpaceExA", reinterpret_cast<void*>(&hle_get_disk_free_space_ex_a));
    register_function("KERNEL32.DLL", "GetLogicalDrives", reinterpret_cast<void*>(&hle_get_logical_drives));
    register_function("KERNEL32.DLL", "GetTempFileNameW", reinterpret_cast<void*>(&hle_get_temp_file_name_w));
    register_function("KERNEL32.DLL", "ReplaceFileW", reinterpret_cast<void*>(&hle_replace_file_w));
    register_function("KERNEL32.DLL", "MoveFileExW", reinterpret_cast<void*>(&hle_move_file_ex_w));
    register_function("KERNEL32.DLL", "RemoveDirectoryW", reinterpret_cast<void*>(&hle_remove_directory_w));
    register_function("KERNEL32.DLL", "GetDriveTypeW", reinterpret_cast<void*>(&hle_get_drive_type_w));
    register_function("KERNEL32.DLL", "GetStdHandle", reinterpret_cast<void*>(&hle_get_std_handle));
    register_function("KERNEL32.DLL", "SetStdHandle", reinterpret_cast<void*>(&hle_set_std_handle));
    register_function("KERNEL32.DLL", "SetEndOfFile", reinterpret_cast<void*>(&hle_set_end_of_file));
    register_function("KERNEL32.DLL", "FlushFileBuffers", reinterpret_cast<void*>(&hle_flush_file_buffers));
    register_function("KERNEL32.DLL", "PeekNamedPipe", reinterpret_cast<void*>(&hle_peek_named_pipe));
    register_function("KERNEL32.DLL", "CreatePipe", reinterpret_cast<void*>(&hle_create_pipe));
    register_function("KERNEL32.DLL", "SetHandleInformation", reinterpret_cast<void*>(&hle_set_handle_information));
    register_function("KERNEL32.DLL", "GetExitCodeProcess", reinterpret_cast<void*>(&hle_get_exit_code_process));
    register_function("KERNEL32.DLL", "GetExitCodeThread", reinterpret_cast<void*>(&hle_get_exit_code_thread));
    register_function("KERNEL32.DLL", "OpenProcess", reinterpret_cast<void*>(&hle_open_process));
    register_function("KERNEL32.DLL", "CreateProcessW", reinterpret_cast<void*>(&hle_create_process_w));
    register_function("KERNEL32.DLL", "GetEnvironmentStringsW", reinterpret_cast<void*>(&hle_get_environment_strings_w));
    register_function("KERNEL32.DLL", "FreeEnvironmentStringsW", reinterpret_cast<void*>(&hle_free_environment_strings_w));
    register_function("KERNEL32.DLL", "SetEnvironmentVariableW", reinterpret_cast<void*>(&hle_set_environment_variable_w));
    register_function("KERNEL32.DLL", "SetEnvironmentVariableA", reinterpret_cast<void*>(&hle_set_environment_variable_a));
    register_function("KERNEL32.DLL", "GetEnvironmentVariableW", reinterpret_cast<void*>(&hle_get_environment_variable_w));
    register_function("KERNEL32.DLL", "GetOEMCP", reinterpret_cast<void*>(&hle_get_oemcp));
    register_function("KERNEL32.DLL", "GetACP", reinterpret_cast<void*>(&hle_get_acp));
    register_function("KERNEL32.DLL", "IsValidCodePage", reinterpret_cast<void*>(&hle_is_valid_code_page));
    register_function("KERNEL32.DLL", "IsValidLocale", reinterpret_cast<void*>(&hle_is_valid_locale));
    register_function("KERNEL32.DLL", "EnumSystemLocalesW", reinterpret_cast<void*>(&hle_enum_system_locales_w));
    register_function("KERNEL32.DLL", "GetStringTypeW", reinterpret_cast<void*>(&hle_get_string_type_w));
    register_function("KERNEL32.DLL", "GetCPInfo", reinterpret_cast<void*>(&hle_get_cpinfo));
    register_function("KERNEL32.DLL", "SetThreadIdealProcessor", reinterpret_cast<void*>(&hle_set_thread_ideal_processor));
    register_function("KERNEL32.DLL", "SetThreadAffinityMask", reinterpret_cast<void*>(&hle_set_thread_affinity_mask));
    register_function("KERNEL32.DLL", "SetThreadPriority", reinterpret_cast<void*>(&hle_set_thread_priority));
    register_function("KERNEL32.DLL", "SetPriorityClass", reinterpret_cast<void*>(&hle_set_priority_class));
    register_function("KERNEL32.DLL", "PowerCreateRequest", reinterpret_cast<void*>(&hle_power_create_request));
    register_function("KERNEL32.DLL", "PowerSetRequest", reinterpret_cast<void*>(&hle_power_set_request));
    register_function("KERNEL32.DLL", "PowerClearRequest", reinterpret_cast<void*>(&hle_power_clear_request));
    register_function("KERNEL32.DLL", "InitializeSRWLock", reinterpret_cast<void*>(&hle_initialize_srw_lock));
    register_function("KERNEL32.DLL", "AcquireSRWLockExclusive", reinterpret_cast<void*>(&hle_acquire_srw_lock_exclusive));
    register_function("KERNEL32.DLL", "ReleaseSRWLockExclusive", reinterpret_cast<void*>(&hle_release_srw_lock_exclusive));
    register_function("KERNEL32.DLL", "TryAcquireSRWLockExclusive", reinterpret_cast<void*>(&hle_try_acquire_srw_lock_exclusive));
    register_function("KERNEL32.DLL", "AcquireSRWLockShared", reinterpret_cast<void*>(&hle_acquire_srw_lock_shared));
    register_function("KERNEL32.DLL", "ReleaseSRWLockShared", reinterpret_cast<void*>(&hle_release_srw_lock_shared));
    register_function("KERNEL32.DLL", "InitializeConditionVariable", reinterpret_cast<void*>(&hle_initialize_condition_variable));
    register_function("KERNEL32.DLL", "WakeConditionVariable", reinterpret_cast<void*>(&hle_wake_condition_variable));
    register_function("KERNEL32.DLL", "WakeAllConditionVariable", reinterpret_cast<void*>(&hle_wake_all_condition_variable));
    register_function("KERNEL32.DLL", "SleepConditionVariableCS", reinterpret_cast<void*>(&hle_sleep_condition_variable_cs));
    register_function("KERNEL32.DLL", "SleepConditionVariableSRW", reinterpret_cast<void*>(&hle_sleep_condition_variable_srw));
    register_function("KERNEL32.DLL", "InitOnceBeginInitialize", reinterpret_cast<void*>(&hle_init_once_begin_initialize));
    register_function("KERNEL32.DLL", "InitOnceComplete", reinterpret_cast<void*>(&hle_init_once_complete));
    register_function("KERNEL32.DLL", "InitializeSListHead", reinterpret_cast<void*>(&hle_initialize_slist_head));
    register_function("KERNEL32.DLL", "InterlockedPushEntrySList", reinterpret_cast<void*>(&hle_interlocked_push_entry_slist));
    register_function("KERNEL32.DLL", "GlobalAlloc", reinterpret_cast<void*>(&hle_global_alloc));
    register_function("KERNEL32.DLL", "GlobalFree", reinterpret_cast<void*>(&hle_global_free));
    register_function("KERNEL32.DLL", "GlobalLock", reinterpret_cast<void*>(&hle_global_lock));
    register_function("KERNEL32.DLL", "GlobalUnlock", reinterpret_cast<void*>(&hle_global_unlock));
    register_function("KERNEL32.DLL", "LocalAlloc", reinterpret_cast<void*>(&hle_global_alloc));
    register_function("KERNEL32.DLL", "LocalFree", reinterpret_cast<void*>(&hle_local_free));
    register_function("KERNEL32.DLL", "SecureZeroMemory", reinterpret_cast<void*>(&hle_secure_zero_memory));
    register_function("KERNEL32.DLL", "GetSystemDefaultLangID", reinterpret_cast<void*>(&hle_get_system_default_lang_id));
    register_function("KERNEL32.DLL", "GetUserDefaultLangID", reinterpret_cast<void*>(&hle_get_user_default_lang_id));
    register_function("KERNEL32.DLL", "GetThreadLocale", reinterpret_cast<void*>(&hle_get_thread_locale));
    register_function("KERNEL32.DLL", "FindFirstFileA", reinterpret_cast<void*>(&hle_find_first_file_a));
    register_function("KERNEL32.DLL", "FindFirstFileW", reinterpret_cast<void*>(&hle_find_first_file_w));
    register_function("KERNEL32.DLL", "FindFirstFileExW", reinterpret_cast<void*>(&hle_find_first_file_ex_w));
    register_function("KERNEL32.DLL", "FindNextFileA", reinterpret_cast<void*>(&hle_find_next_file_a));
    register_function("KERNEL32.DLL", "FindNextFileW", reinterpret_cast<void*>(&hle_find_next_file_w));
    register_function("KERNEL32.DLL", "FindClose", reinterpret_cast<void*>(&hle_find_close));

    register_function("KERNEL32.DLL", "IsDBCSLeadByteEx", reinterpret_cast<void*>(&hle_isdbcs_lead_byte_ex));
    register_function("KERNEL32.DLL", "__C_specific_handler", reinterpret_cast<void*>(&hle_msvcrt___C_specific_handler));
    register_function("KERNEL32.DLL", "GetProcAddress", reinterpret_cast<void*>(&hle_get_proc_address));
    register_function("KERNEL32.DLL", "GetModuleHandleA", reinterpret_cast<void*>(&hle_get_module_handle_a));
    register_function("KERNEL32.DLL", "GetModuleHandleW", reinterpret_cast<void*>(&hle_get_module_handle_w));
    register_function("KERNEL32.DLL", "GetModuleHandleExW", reinterpret_cast<void*>(&hle_get_module_handle_w));
    register_function("KERNEL32.DLL", "LoadLibraryA", reinterpret_cast<void*>(&hle_load_library_a));
    register_function("KERNEL32.DLL", "LoadLibraryW", reinterpret_cast<void*>(&hle_load_library_w));
    register_function("KERNEL32.DLL", "LoadLibraryExW", reinterpret_cast<void*>(&hle_load_library_w));
    register_function("KERNEL32.DLL", "FreeLibrary", reinterpret_cast<void*>(&hle_free_library));
    register_function("KERNEL32.DLL", "FreeLibraryAndExitThread", reinterpret_cast<void*>(&hle_exit_thread));
    register_function("KERNEL32.DLL", "GetModuleFileNameA", reinterpret_cast<void*>(&hle_get_module_file_name_a));
    register_function("KERNEL32.DLL", "GetModuleFileNameW", reinterpret_cast<void*>(&hle_get_module_file_name_w));
    register_function("KERNEL32.DLL", "EncodePointer", reinterpret_cast<void*>(&hle_encode_pointer));
    register_function("KERNEL32.DLL", "DecodePointer", reinterpret_cast<void*>(&hle_decode_pointer));
    register_function("KERNEL32.DLL", "EncodeSystemPointer", reinterpret_cast<void*>(&hle_encode_system_pointer));
    register_function("KERNEL32.DLL", "DecodeSystemPointer", reinterpret_cast<void*>(&hle_decode_system_pointer));

    register_function("KERNEL32.DLL", "GetSystemInfo", reinterpret_cast<void*>(&hle_get_system_info));
    register_function("KERNEL32.DLL", "GetNativeSystemInfo", reinterpret_cast<void*>(&hle_get_native_system_info));
    register_function("KERNEL32.DLL", "IsProcessorFeaturePresent", reinterpret_cast<void*>(&hle_is_processor_feature_present));
    register_function("KERNEL32.DLL", "GetCommandLineA", reinterpret_cast<void*>(&hle_get_command_line_a));
    register_function("KERNEL32.DLL", "GetCommandLineW", reinterpret_cast<void*>(&hle_get_command_line_w));
    register_function("KERNEL32.DLL", "GetEnvironmentVariableA", reinterpret_cast<void*>(&hle_get_environment_variable_a));
    register_function("KERNEL32.DLL", "QueryPerformanceCounter", reinterpret_cast<void*>(&hle_query_performance_counter));
    register_function("KERNEL32.DLL", "QueryPerformanceFrequency", reinterpret_cast<void*>(&hle_query_performance_frequency));
    register_function("KERNEL32.DLL", "GetTickCount", reinterpret_cast<void*>(&hle_get_tick_count));
    register_function("KERNEL32.DLL", "lstrcpyA", reinterpret_cast<void*>(&hle_lstrcpy_a));
    register_function("KERNEL32.DLL", "lstrcatA", reinterpret_cast<void*>(&hle_lstrcat_a));
    register_function("KERNEL32.DLL", "lstrlenA", reinterpret_cast<void*>(&hle_lstrlen_a));
    register_function("KERNEL32.DLL", "wcslen", reinterpret_cast<void*>(&hle_wcslen));
    register_function("KERNEL32.DLL", "GetProcessId", reinterpret_cast<void*>(&hle_get_process_id));
    register_function("KERNEL32.DLL", "GetHandleInformation", reinterpret_cast<void*>(&hle_get_handle_information));
    register_function("KERNEL32.DLL", "lstrcmpA", reinterpret_cast<void*>(&hle_lstrcmp_a));
    register_function("KERNEL32.DLL", "GetThreadPriority", reinterpret_cast<void*>(&hle_get_thread_priority));
    register_function("KERNEL32.DLL", "GetPrivateProfileStringA", reinterpret_cast<void*>(&hle_get_private_profile_string_a));
    register_function("KERNEL32.DLL", "WritePrivateProfileStringA", reinterpret_cast<void*>(&hle_write_private_profile_string_a));
    register_function("KERNEL32.DLL", "GetTickCount64", reinterpret_cast<void*>(&hle_get_tick_count_64));
    register_function("KERNEL32.DLL", "GetLastError", reinterpret_cast<void*>(&hle_get_last_error));
    register_function("KERNEL32.DLL", "SetLastError", reinterpret_cast<void*>(&hle_set_last_error));
    register_function("KERNEL32.DLL", "GetStartupInfoA", reinterpret_cast<void*>(&hle_get_startup_info_a));
    register_function("KERNEL32.DLL", "GetStartupInfoW", reinterpret_cast<void*>(&hle_get_startup_info_w));
    register_function("api-ms-win-core-processthreads-l1-1-0.dll", "GetStartupInfoW", reinterpret_cast<void*>(&hle_get_startup_info_w));
    register_function("api-ms-win-core-processthreads-l1-1-1.dll", "GetStartupInfoW", reinterpret_cast<void*>(&hle_get_startup_info_w));
    register_function("api-ms-win-core-processthreads-l1-1-2.dll", "GetStartupInfoW", reinterpret_cast<void*>(&hle_get_startup_info_w));
    register_function("KERNEL32.DLL", "SetUnhandledExceptionFilter", reinterpret_cast<void*>(&hle_set_unhandled_exception_filter));
    register_function("KERNEL32.DLL", "UnhandledExceptionFilter", reinterpret_cast<void*>(&hle_unhandled_exception_filter));
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
    register_function("msvcrt.dll", "fflush",   reinterpret_cast<void*>(&hle_msvcrt_fflush));
    register_function("msvcrt.dll", "strerror", reinterpret_cast<void*>(&hle_msvcrt_strerror));
    register_function("msvcrt.dll", "localeconv", reinterpret_cast<void*>(&hle_msvcrt_localeconv));
    register_function("msvcrt.dll", "_lock",    reinterpret_cast<void*>(&hle_msvcrt_lock));
    register_function("msvcrt.dll", "_unlock",  reinterpret_cast<void*>(&hle_msvcrt_unlock));
    register_function("msvcrt.dll", "___lc_codepage_func", reinterpret_cast<void*>(&hle_msvcrt_lc_codepage_func));
    register_function("msvcrt.dll", "___mb_cur_max_func", reinterpret_cast<void*>(&hle_msvcrt_mb_cur_max_func));
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
    register_function("USER32.DLL", "wsprintfA", reinterpret_cast<void*>(&hle_user32_wsprintf_a));
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
    register_function("USER32.DLL", "RegisterWindowMessageA", reinterpret_cast<void*>(&hle_register_window_message_a));
    register_function("USER32.DLL", "LoadIconA", reinterpret_cast<void*>(&hle_load_icon_a));
    register_function("USER32.DLL", "LoadCursorA", reinterpret_cast<void*>(&hle_load_cursor_a));
    register_function("USER32.DLL", "GetWindowLongA", reinterpret_cast<void*>(&hle_get_window_long_a));
    register_function("USER32.DLL", "SetWindowLongA", reinterpret_cast<void*>(&hle_set_window_long_a));
    register_function("USER32.DLL", "GetWindowLongPtrA", reinterpret_cast<void*>(&hle_get_window_long_a));
    register_function("USER32.DLL", "SetWindowLongPtrA", reinterpret_cast<void*>(&hle_set_window_long_a));
    register_function("USER32.DLL", "GetWindowLongW", reinterpret_cast<void*>(&hle_get_window_long_a));
    register_function("USER32.DLL", "SetWindowLongW", reinterpret_cast<void*>(&hle_set_window_long_a));
    register_function("USER32.DLL", "GetWindowLongPtrW", reinterpret_cast<void*>(&hle_get_window_long_a));
    register_function("USER32.DLL", "SetWindowLongPtrW", reinterpret_cast<void*>(&hle_set_window_long_a));
    register_function("USER32.DLL", "SystemParametersInfoA", reinterpret_cast<void*>(&hle_system_parameters_info_a));
    register_function("USER32.DLL", "EnumWindows", reinterpret_cast<void*>(&hle_enum_windows));
    register_function("USER32.DLL", "GetDoubleClickTime", reinterpret_cast<void*>(&hle_get_double_click_time));
    register_function("USER32.DLL", "GetKeyboardType", reinterpret_cast<void*>(&hle_get_keyboard_type));
    register_function("USER32.DLL", "SetTimer", reinterpret_cast<void*>(&hle_set_timer));
    register_function("USER32.DLL", "KillTimer", reinterpret_cast<void*>(&hle_kill_timer));
    register_function("USER32.DLL", "MonitorFromWindow", reinterpret_cast<void*>(&hle_monitor_from_window));
    register_function("USER32.DLL", "GetMonitorInfoA", reinterpret_cast<void*>(&hle_get_monitor_info_a));
    register_function("USER32.DLL", "EnumDisplayMonitors", reinterpret_cast<void*>(&hle_enum_display_monitors));
    register_function("USER32.DLL", "GetDesktopWindow", reinterpret_cast<void*>(&hle_get_desktop_window));
    register_function("USER32.DLL", "ClientToScreen", reinterpret_cast<void*>(&hle_client_to_screen));
    register_function("USER32.DLL", "ScreenToClient", reinterpret_cast<void*>(&hle_screen_to_client));
    register_function("GDI32.DLL", "CreateFontIndirectA", reinterpret_cast<void*>(&hle_create_font_indirect_a));
    register_function("USER32.DLL", "MapVirtualKeyA", reinterpret_cast<void*>(&hle_map_virtual_key_a));
    register_function("USER32.DLL", "GetDC", reinterpret_cast<void*>(&hle_get_dc));
    register_function("USER32.DLL", "ReleaseDC", reinterpret_cast<void*>(&hle_release_dc));
    register_function("USER32.DLL", "BeginPaint", reinterpret_cast<void*>(&hle_begin_paint));
    register_function("USER32.DLL", "EndPaint", reinterpret_cast<void*>(&hle_end_paint));
    register_function("USER32.DLL", "InvalidateRect", reinterpret_cast<void*>(&hle_invalidate_rect));
    register_function("USER32.DLL", "MessageBoxA", reinterpret_cast<void*>(&hle_message_box_a));
    register_function("USER32.DLL", "MessageBoxW", reinterpret_cast<void*>(&hle_message_box_w));

    register_function("GDI32.DLL", "GetPixel", reinterpret_cast<void*>(&hle_get_pixel));
    register_function("GDI32.DLL", "GetDIBits", reinterpret_cast<void*>(&hle_get_dibits));
    register_function("GDI32.DLL", "GetDeviceCaps", reinterpret_cast<void*>(&hle_get_device_caps));

    // AVRT.DLL & DWMAPI.DLL & DWrite.DLL
    register_function("AVRT.DLL", "AvSetMmThreadCharacteristicsW", reinterpret_cast<void*>(&hle_av_set_mm_thread_characteristics_w));
    register_function("AVRT.DLL", "AvSetMmThreadPriority", reinterpret_cast<void*>(&hle_av_set_mm_thread_priority));
    register_function("dwmapi.dll", "DwmEnableBlurBehindWindow", reinterpret_cast<void*>(&hle_dwm_enable_blur_behind_window));
    register_function("dwmapi.dll", "DwmSetWindowAttribute", reinterpret_cast<void*>(&hle_dwm_set_window_attribute));
    register_function("DWrite.dll", "DWriteCreateFactory", reinterpret_cast<void*>(&hle_dwrite_create_factory));

    // BCRYPT.DLL & CRYPT32.DLL
    register_function("bcrypt.dll", "BCryptGenRandom", reinterpret_cast<void*>(&hle_bcrypt_gen_random));
    register_function("CRYPT32.dll", "CertOpenSystemStoreA", reinterpret_cast<void*>(&hle_cert_open_system_store_a));
    register_function("CRYPT32.dll", "CertCloseStore", reinterpret_cast<void*>(&hle_cert_close_store));
    register_function("CRYPT32.dll", "CertGetCertificateContextProperty", reinterpret_cast<void*>(&hle_cert_get_certificate_context_property));
    register_function("CRYPT32.dll", "CryptBinaryToStringA", reinterpret_cast<void*>(&hle_crypt_binary_to_string_a));
    register_function("CRYPT32.dll", "CertEnumCertificatesInStore", reinterpret_cast<void*>(&hle_cert_enum_certificates_in_store));

    // ADVAPI32.DLL & SHELL32.DLL
    register_function("ADVAPI32.dll", "OpenProcessToken", reinterpret_cast<void*>(&hle_open_process_token));
    register_function("ADVAPI32.dll", "GetTokenInformation", reinterpret_cast<void*>(&hle_get_token_information));
    register_function("ADVAPI32.dll", "RegEnumValueW", reinterpret_cast<void*>(&hle_reg_enum_value_w));
    register_function("ADVAPI32.dll", "RegEnumValueA", reinterpret_cast<void*>(&hle_reg_enum_value_a));
    register_function("ADVAPI32.dll", "GetCurrentHwProfileA", reinterpret_cast<void*>(&hle_get_current_hw_profile_a));
    register_function("ADVAPI32.dll", "LookupPrivilegeValueW", reinterpret_cast<void*>(&hle_lookup_privilege_value_w));
    register_function("ADVAPI32.dll", "AdjustTokenPrivileges", reinterpret_cast<void*>(&hle_adjust_token_privileges));
    register_function("ADVAPI32.dll", "GetSidSubAuthority", reinterpret_cast<void*>(&hle_get_sid_sub_authority));
    register_function("ADVAPI32.dll", "GetSidSubAuthorityCount", reinterpret_cast<void*>(&hle_get_sid_sub_authority_count));

    register_function("SHELL32.dll", "ShellExecuteW", reinterpret_cast<void*>(&hle_shell_execute_w));
    register_function("SHELL32.dll", "CommandLineToArgvW", reinterpret_cast<void*>(&hle_command_line_to_argv_w));
    register_function("SHELL32.dll", "SHFileOperationW", reinterpret_cast<void*>(&hle_sh_file_operation_w));
    register_function("SHELL32.dll", "DragAcceptFiles", reinterpret_cast<void*>(&hle_drag_accept_files));
    register_function("SHELL32.dll", "DragQueryFileW", reinterpret_cast<void*>(&hle_drag_query_file_w));

    // IMM32.DLL
    register_function("IMM32.dll", "ImmGetContext", reinterpret_cast<void*>(&hle_imm_get_context));
    register_function("IMM32.dll", "ImmReleaseContext", reinterpret_cast<void*>(&hle_imm_release_context));
    register_function("IMM32.dll", "ImmSetCandidateWindow", reinterpret_cast<void*>(&hle_imm_set_candidate_window));
    register_function("IMM32.dll", "ImmGetCompositionStringW", reinterpret_cast<void*>(&hle_imm_get_composition_string_w));
    register_function("IMM32.dll", "ImmSetCompositionWindow", reinterpret_cast<void*>(&hle_imm_set_composition_window));
    register_function("IMM32.dll", "ImmAssociateContext", reinterpret_cast<void*>(&hle_imm_associate_context));

    // OPENGL32.DLL
    register_function("OPENGL32.dll", "wglCreateContext", reinterpret_cast<void*>(&hle_wgl_create_context));
    register_function("OPENGL32.dll", "wglDeleteContext", reinterpret_cast<void*>(&hle_wgl_delete_context));
    register_function("OPENGL32.dll", "wglGetProcAddress", reinterpret_cast<void*>(&hle_wgl_get_proc_address));
    register_function("OPENGL32.dll", "wglMakeCurrent", reinterpret_cast<void*>(&hle_wgl_make_current));

    // WS2_32.DLL / WSOCK32.DLL
    register_function("WS2_32.dll", "WSAConnect", reinterpret_cast<void*>(&hle_wsaconnect));
    register_function("WS2_32.dll", "getaddrinfo", reinterpret_cast<void*>(&hle_getaddrinfo));
    register_function("WS2_32.dll", "freeaddrinfo", reinterpret_cast<void*>(&hle_freeaddrinfo));
    register_function("WS2_32.dll", "getnameinfo", reinterpret_cast<void*>(&hle_getnameinfo));
    register_function("WS2_32.dll", "inet_pton", reinterpret_cast<void*>(&hle_inet_pton));

    // XINPUT1_4.DLL / XINPUT9_1_0.DLL
    register_function("XINPUT1_4.DLL", "XInputGetState", reinterpret_cast<void*>(&hle_xinput_get_state));
    register_function("XINPUT1_4.DLL", "XInputSetState", reinterpret_cast<void*>(&hle_xinput_set_state));
    register_function("XINPUT1_4.DLL", "XInputGetCapabilities", reinterpret_cast<void*>(&hle_xinput_get_capabilities));
    register_function("XINPUT9_1_0.DLL", "XInputGetState", reinterpret_cast<void*>(&hle_xinput_get_state));
    register_function("XINPUT9_1_0.DLL", "XInputSetState", reinterpret_cast<void*>(&hle_xinput_set_state));
    register_function("XINPUT9_1_0.DLL", "XInputGetCapabilities", reinterpret_cast<void*>(&hle_xinput_get_capabilities));

    // XInput 1.3 (the common gamepad API most controller games import)
    register_function("XINPUT1_3.DLL", "XInputGetState", reinterpret_cast<void*>(&hle_xinput_get_state));
    register_function("XINPUT1_3.DLL", "XInputSetState", reinterpret_cast<void*>(&hle_xinput_set_state));
    register_function("XINPUT1_3.DLL", "XInputGetCapabilities", reinterpret_cast<void*>(&hle_xinput_get_capabilities));
    register_function("XINPUT1_3.DLL", "XInputEnable", reinterpret_cast<void*>(&hle_xinput_enable));
    register_function("XINPUT1_3.DLL", "XInputGetBatteryInformation", reinterpret_cast<void*>(&hle_xinput_get_battery_info));
    register_function("XINPUT1_3.DLL", "XInputGetKeystroke", reinterpret_cast<void*>(&hle_xinput_get_keystroke));
    register_function("XINPUT1_3.DLL", "XInputGetDSoundAudioDeviceGuids", reinterpret_cast<void*>(&hle_xinput_get_dsound_audio_device_guids));

    // XInput 1.4 extra
    register_function("XINPUT1_4.DLL", "XInputEnable", reinterpret_cast<void*>(&hle_xinput_enable));
    register_function("XINPUT1_4.DLL", "XInputGetBatteryInformation", reinterpret_cast<void*>(&hle_xinput_get_battery_info));
    register_function("XINPUT1_4.DLL", "XInputGetKeystroke", reinterpret_cast<void*>(&hle_xinput_get_keystroke));
    register_function("XINPUT1_4.DLL", "XInputGetAudioDeviceIds", reinterpret_cast<void*>(&hle_xinput_get_audio_device_ids));

    // STEAM_API64.DLL / STEAM_API.DLL
    register_function("STEAM_API64.DLL", "SteamAPI_Init", reinterpret_cast<void*>(&hle_steam_api_init));
    register_function("STEAM_API64.DLL", "SteamAPI_InitEx", reinterpret_cast<void*>(&hle_steam_api_init_ex));
    register_function("STEAM_API64.DLL", "SteamAPI_InitFlat", reinterpret_cast<void*>(&hle_steam_api_init_flat));
    register_function("STEAM_API64.DLL", "SteamAPI_Shutdown", reinterpret_cast<void*>(&hle_steam_api_shutdown));
    register_function("STEAM_API64.DLL", "SteamAPI_RunCallbacks", reinterpret_cast<void*>(&hle_steam_api_run_callbacks));
    register_function("STEAM_API64.DLL", "SteamAPI_RestartAppIfNecessary", reinterpret_cast<void*>(&hle_steam_api_restart_app_if_necessary));
    register_function("STEAM_API64.DLL", "SteamInternal_CreateInterface", reinterpret_cast<void*>(&hle_steam_internal_create_interface));
    register_function("STEAM_API64.DLL", "SteamInternal_SteamAPI_Init", reinterpret_cast<void*>(&hle_steam_api_init));
    register_function("STEAM_API64.DLL", "SteamInternal_ContextInit", reinterpret_cast<void*>(&hle_steam_internal_context_init));
    register_function("STEAM_API64.DLL", "SteamInternal_FindOrCreateUserInterface", reinterpret_cast<void*>(&hle_steam_internal_find_or_create_user_interface));
    register_function("STEAM_API64.DLL", "SteamInternal_FindOrCreateGameServerInterface", reinterpret_cast<void*>(&hle_steam_internal_find_or_create_server_interface));
    register_function("STEAM_API64.DLL", "SteamAPI_RegisterCallback", reinterpret_cast<void*>(&hle_steam_register_callback));
    register_function("STEAM_API64.DLL", "SteamAPI_UnregisterCallback", reinterpret_cast<void*>(&hle_steam_unregister_callback));
    register_function("STEAM_API64.DLL", "SteamAPI_RegisterCallResult", reinterpret_cast<void*>(&hle_steam_register_call_result));
    register_function("STEAM_API64.DLL", "SteamAPI_UnregisterCallResult", reinterpret_cast<void*>(&hle_steam_unregister_call_result));
    register_function("STEAM_API64.DLL", "SteamAPI_IsSteamRunning", reinterpret_cast<void*>(&hle_steam_is_running));
    register_function("STEAM_API64.DLL", "SteamAPI_GetHSteamUser", reinterpret_cast<void*>(&hle_steam_get_h_steam_user));
    register_function("STEAM_API64.DLL", "SteamAPI_GetHSteamPipe", reinterpret_cast<void*>(&hle_steam_get_h_steam_user));
    register_function("STEAM_API64.DLL", "SteamGameServer_GetHSteamUser", reinterpret_cast<void*>(&hle_steam_get_h_steam_user));
    register_function("STEAM_API64.DLL", "SteamClient", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamUser", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamFriends", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamUtils", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamUserStats", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamApps", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamNetworking", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamNetworkingSockets", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamMatchmaking", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamRemoteStorage", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamAPI_SteamClient_v020", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamAPI_SteamUser_v021", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamAPI_SteamFriends_v017", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamAPI_SteamUtils_v010", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamAPI_SteamUserStats_v012", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamAPI_SteamApps_v008", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API64.DLL", "SteamAPI_SteamRemoteStorage_v016", reinterpret_cast<void*>(&hle_steam_accessor));

    register_function("STEAM_API.DLL", "SteamAPI_Init", reinterpret_cast<void*>(&hle_steam_api_init));
    register_function("STEAM_API.DLL", "SteamAPI_InitEx", reinterpret_cast<void*>(&hle_steam_api_init_ex));
    register_function("STEAM_API.DLL", "SteamAPI_InitFlat", reinterpret_cast<void*>(&hle_steam_api_init_flat));
    register_function("STEAM_API.DLL", "SteamAPI_Shutdown", reinterpret_cast<void*>(&hle_steam_api_shutdown));
    register_function("STEAM_API.DLL", "SteamAPI_RunCallbacks", reinterpret_cast<void*>(&hle_steam_api_run_callbacks));
    register_function("STEAM_API.DLL", "SteamAPI_RestartAppIfNecessary", reinterpret_cast<void*>(&hle_steam_api_restart_app_if_necessary));
    register_function("STEAM_API.DLL", "SteamAPI_IsSteamRunning", reinterpret_cast<void*>(&hle_steam_is_running));
    register_function("STEAM_API.DLL", "SteamUser", reinterpret_cast<void*>(&hle_steam_accessor));
    register_function("STEAM_API.DLL", "SteamFriends", reinterpret_cast<void*>(&hle_steam_accessor));

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

    // Registry
    register_function("ADVAPI32.dll", "RegOpenKeyExA",     reinterpret_cast<void*>(&hle_reg_open_key_ex_a));
    register_function("ADVAPI32.dll", "RegOpenKeyExW",     reinterpret_cast<void*>(&hle_reg_open_key_ex_w));
    register_function("ADVAPI32.dll", "RegQueryValueExA",  reinterpret_cast<void*>(&hle_reg_query_value_ex_a));
    register_function("ADVAPI32.dll", "RegQueryValueExW",  reinterpret_cast<void*>(&hle_reg_query_value_ex_w));
    register_function("ADVAPI32.dll", "RegCreateKeyExA",   reinterpret_cast<void*>(&hle_reg_create_key_ex_a));
    register_function("ADVAPI32.dll", "RegCreateKeyExW",   reinterpret_cast<void*>(&hle_reg_create_key_ex_w));
    register_function("ADVAPI32.dll", "RegSetValueExA",    reinterpret_cast<void*>(&hle_reg_set_value_ex_a));
    register_function("ADVAPI32.dll", "RegSetValueExW",    reinterpret_cast<void*>(&hle_reg_set_value_ex_w));
    register_function("ADVAPI32.dll", "RegCloseKey",       reinterpret_cast<void*>(&hle_reg_close_key));
    register_function("ADVAPI32.dll", "RegDeleteValueA",   reinterpret_cast<void*>(&hle_reg_delete_value_a));
    register_function("ADVAPI32.dll", "RegGetValueA",      reinterpret_cast<void*>(&hle_reg_get_value_a));
    register_function("ADVAPI32.dll", "RegGetValueW",      reinterpret_cast<void*>(&hle_reg_get_value_a));
    register_function("ADVAPI32.dll", "RegDisablePredefinedCache", reinterpret_cast<void*>(&hle_reg_disable_predefined_cache));
    // Seed the in-memory registry with standard keys.
    registry_seed();

    // File system additions
    register_function("KERNEL32.DLL", "CreateDirectoryA",  reinterpret_cast<void*>(&hle_create_directory_a));
    register_function("KERNEL32.DLL", "RemoveDirectoryA",  reinterpret_cast<void*>(&hle_remove_directory_a));
    register_function("KERNEL32.DLL", "DeleteFileA",       reinterpret_cast<void*>(&hle_delete_file_a));
    register_function("KERNEL32.DLL", "CopyFileA",         reinterpret_cast<void*>(&hle_copy_file_a));
    register_function("KERNEL32.DLL", "CopyFileW",         reinterpret_cast<void*>(&hle_copy_file_w));
    register_function("KERNEL32.DLL", "OpenFile",          reinterpret_cast<void*>(&hle_open_file));
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
    register_function("KERNEL32.DLL", "EncodePointer",          reinterpret_cast<void*>(&hle_encode_pointer));
    register_function("KERNEL32.DLL", "DecodePointer",          reinterpret_cast<void*>(&hle_decode_pointer));
    register_function("KERNEL32.DLL", "GetCurrentProcessorNumber", reinterpret_cast<void*>(&hle_get_current_processor_number));
    register_function("KERNEL32.DLL", "InitializeSListHead",    reinterpret_cast<void*>(&hle_initialize_slist_head));
    register_function("KERNEL32.DLL", "InterlockedFlushSList",  reinterpret_cast<void*>(&hle_interlocked_flush_slist));

    // WINMM.DLL
    register_function("WINMM.DLL", "timeGetTime",         reinterpret_cast<void*>(&hle_time_get_time));
    register_function("WINMM.DLL", "timeBeginPeriod", reinterpret_cast<void*>(&hle_time_begin_period));
       register_function("WINMM.DLL", "timeGetDevCaps", reinterpret_cast<void*>(&hle_time_get_dev_caps));
    register_function("WINMM.DLL", "timeEndPeriod",       reinterpret_cast<void*>(&hle_time_end_period));
    register_function("WINMM.DLL", "PlaySoundA",          reinterpret_cast<void*>(&hle_play_sound_a));
    register_function("WINMM.DLL", "PlaySoundW",          reinterpret_cast<void*>(&hle_play_sound_w));
    register_function("WINMM.DLL", "waveOutGetNumDevs",   reinterpret_cast<void*>(&hle_wave_out_get_num_devs));
    register_function("WINMM.DLL", "waveOutOpen",         reinterpret_cast<void*>(&hle_wave_out_open));
    register_function("WINMM.DLL", "waveOutWrite",        reinterpret_cast<void*>(&hle_wave_out_write));
    register_function("WINMM.DLL", "waveOutClose",        reinterpret_cast<void*>(&hle_wave_out_close));
    register_function("WINMM.DLL", "waveOutSetVolume",    reinterpret_cast<void*>(&hle_wave_out_set_volume));
    register_function("WINMM.DLL", "waveInGetNumDevs",    reinterpret_cast<void*>(&hle_wave_in_get_num_devs));

    // mmsystem joystick + MCI + MIDI (game-facing)
    register_function("WINMM.DLL", "joyGetNumDevs",       reinterpret_cast<void*>(&hle_joy_get_num_devs));
    register_function("WINMM.DLL", "joyGetPosEx",         reinterpret_cast<void*>(&hle_joy_get_pos_ex));
    register_function("WINMM.DLL", "joyGetDevCapsA",      reinterpret_cast<void*>(&hle_joy_get_dev_caps_a));
    register_function("WINMM.DLL", "joyGetDevCapsW",      reinterpret_cast<void*>(&hle_joy_get_dev_caps_a));
    register_function("WINMM.DLL", "mciSendStringA",      reinterpret_cast<void*>(&hle_mci_send_string_a));
    register_function("WINMM.DLL", "mciSendStringW",      reinterpret_cast<void*>(&hle_mci_send_string_a));
    register_function("WINMM.DLL", "mciGetErrorStringA",  reinterpret_cast<void*>(&hle_mci_get_error_string_a));
    register_function("WINMM.DLL", "midiOutGetNumDevs",   reinterpret_cast<void*>(&hle_wave_out_get_num_devs));
    register_function("WINMM.DLL", "midiOutOpen",         reinterpret_cast<void*>(&hle_wave_out_open));
    register_function("WINMM.DLL", "midiOutShortMsg",     reinterpret_cast<void*>(&hle_midi_out_short_msg));
    register_function("WINMM.DLL", "midiOutClose",        reinterpret_cast<void*>(&hle_wave_out_close));
    register_function("WINMM.DLL", "timeSetEvent",        reinterpret_cast<void*>(&hle_time_set_event));

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
    register_function("GDI32.DLL", "GetDC",                reinterpret_cast<void*>(&hle_get_dc));
    register_function("GDI32.DLL", "ReleaseDC",            reinterpret_cast<void*>(&hle_release_dc));
    register_function("GDI32.DLL", "CreateCompatibleDC",   reinterpret_cast<void*>(&hle_create_compatible_dc));
    register_function("GDI32.DLL", "CreateCompatibleBitmap", reinterpret_cast<void*>(&hle_create_compatible_bitmap));
    register_function("GDI32.DLL", "SelectObject",         reinterpret_cast<void*>(&hle_select_object));
    register_function("GDI32.DLL", "DeleteObject",         reinterpret_cast<void*>(&hle_delete_object));
    register_function("GDI32.DLL", "DeleteDC",             reinterpret_cast<void*>(&hle_delete_dc));
    register_function("GDI32.DLL", "SetPixel",             reinterpret_cast<void*>(&hle_set_pixel));
    register_function("GDI32.DLL", "BitBlt",               reinterpret_cast<void*>(&hle_bit_blt));
    register_function("GDI32.DLL", "GetStockObject",       reinterpret_cast<void*>(&hle_get_stock_object));
    register_function("GDI32.DLL", "GetObjectA",           reinterpret_cast<void*>(&hle_get_object_a));
    register_function("GDI32.DLL", "SetBkColor",           reinterpret_cast<void*>(&hle_set_bk_color));
    register_function("GDI32.DLL", "SetTextColor",         reinterpret_cast<void*>(&hle_set_text_color));
    register_function("GDI32.DLL", "TextOutA",             reinterpret_cast<void*>(&hle_text_out_a));
    register_function("GDI32.DLL", "FillRect",             reinterpret_cast<void*>(&hle_fill_rect));
    register_function("GDI32.DLL", "Rectangle",            reinterpret_cast<void*>(&hle_rectangle));
    register_function("GDI32.DLL", "Ellipse",              reinterpret_cast<void*>(&hle_ellipse));
    register_function("GDI32.DLL", "MoveToEx",             reinterpret_cast<void*>(&hle_move_to_ex));
    register_function("GDI32.DLL", "LineTo",               reinterpret_cast<void*>(&hle_line_to));
    register_function("GDI32.DLL", "CreatePen",            reinterpret_cast<void*>(&hle_create_pen));
    register_function("GDI32.DLL", "CreateSolidBrush",     reinterpret_cast<void*>(&hle_create_solid_brush));
    register_function("GDI32.DLL", "SetBkMode",            reinterpret_cast<void*>(&hle_set_bk_mode));
    register_function("GDI32.DLL", "SetTextAlign",         reinterpret_cast<void*>(&hle_set_text_align));
    register_function("GDI32.DLL", "GetTextExtentPoint32A", reinterpret_cast<void*>(&hle_get_text_extent_point32_a));
    register_function("GDI32.DLL", "GetTextMetricsA",      reinterpret_cast<void*>(&hle_get_text_metrics_a));
    register_function("GDI32.DLL", "DrawTextA",            reinterpret_cast<void*>(&hle_draw_text_a));
    register_function("GDI32.DLL", "ExtTextOutA",          reinterpret_cast<void*>(&hle_ext_text_out_a));

    // OPENGL32.DLL & VULKAN-1.DLL
    register_function("OPENGL32.dll", "wglGetProcAddress", reinterpret_cast<void*>(&hle_wgl_get_proc_address));
    register_function("vulkan-1.dll", "vkGetInstanceProcAddr", reinterpret_cast<void*>(&hle_vk_get_instance_proc_addr));
    register_function("vulkan-1.dll", "vkCreateWin32SurfaceKHR", reinterpret_cast<void*>(&hle_vk_create_win32_surface_khr));
    register_function("vulkan-1.dll", "vkGetPhysicalDeviceWin32PresentationSupportKHR", reinterpret_cast<void*>(&hle_vk_get_physical_device_win32_presentation_support_khr));

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
    register_function("WS2_32.dll", "bind",               reinterpret_cast<void*>(&hle_bind));
    register_function("WS2_32.dll", "listen",             reinterpret_cast<void*>(&hle_listen));
    register_function("WS2_32.dll", "accept",             reinterpret_cast<void*>(&hle_accept));
    register_function("WS2_32.dll", "getsockname",        reinterpret_cast<void*>(&hle_getsockname));
    register_function("WS2_32.dll", "getpeername",        reinterpret_cast<void*>(&hle_getpeername));
    register_function("WS2_32.dll", "setsockopt",         reinterpret_cast<void*>(&hle_setsockopt));
    register_function("WS2_32.dll", "shutdown",           reinterpret_cast<void*>(&hle_shutdown));
    register_function("WS2_32.dll", "inet_addr",          reinterpret_cast<void*>(&hle_inet_addr));
    register_function("WS2_32.dll", "inet_ntoa",          reinterpret_cast<void*>(&hle_inet_ntoa));
    register_function("WS2_32.dll", "select",             reinterpret_cast<void*>(&hle_select));

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
    register_function("USER32.DLL", "CreateWindowExW",    reinterpret_cast<void*>(&hle_create_window_ex_w));
    register_function("USER32.DLL", "CreateWindowW",      reinterpret_cast<void*>(&hle_create_window_ex_w));
    register_function("USER32.DLL", "DefWindowProcW",     reinterpret_cast<void*>(&hle_def_window_proc_w));
    register_function("USER32.DLL", "RegisterClassW",     reinterpret_cast<void*>(&hle_register_class_w));
    register_function("USER32.DLL", "RegisterClassExW",   reinterpret_cast<void*>(&hle_register_class_w));
    register_function("USER32.DLL", "PostMessageW",       reinterpret_cast<void*>(&hle_post_message_w));
    register_function("USER32.DLL", "SendMessageW",       reinterpret_cast<void*>(&hle_send_message_w));
    register_function("USER32.DLL", "SetCursor",          reinterpret_cast<void*>(&hle_set_cursor));
    register_function("USER32.DLL", "GetForegroundWindow",reinterpret_cast<void*>(&hle_get_foreground_window));
    register_function("USER32.DLL", "SetForegroundWindow",reinterpret_cast<void*>(&hle_set_foreground_window));
    register_function("USER32.DLL", "GetActiveWindow",    reinterpret_cast<void*>(&hle_get_active_window));
    register_function("USER32.DLL", "SetActiveWindow",    reinterpret_cast<void*>(&hle_set_active_window));
    register_function("USER32.DLL", "PeekMessageW",       reinterpret_cast<void*>(&hle_peek_message_a));
    register_function("USER32.DLL", "GetMessageW",        reinterpret_cast<void*>(&hle_get_message_a));
    register_function("USER32.DLL", "DispatchMessageW",   reinterpret_cast<void*>(&hle_dispatch_message_a));
    register_function("USER32.DLL", "TranslateMessageW",  reinterpret_cast<void*>(&hle_translate_message));
    register_function("USER32.DLL", "GetCapture",         reinterpret_cast<void*>(&hle_get_capture));
    register_function("USER32.DLL", "SetCapture",         reinterpret_cast<void*>(&hle_set_capture));
    register_function("USER32.DLL", "ReleaseCapture",     reinterpret_cast<void*>(&hle_release_capture));

    // SEH & Exception Handling
    register_function("KERNEL32.DLL", "RtlLookupFunctionEntry", reinterpret_cast<void*>(&hle_rtl_lookup_function_entry));
    register_function("KERNEL32.DLL", "RtlVirtualUnwind",       reinterpret_cast<void*>(&hle_rtl_virtual_unwind));
    register_function("KERNEL32.DLL", "RtlCaptureContext",      reinterpret_cast<void*>(&hle_rtl_capture_context));
    register_function("KERNEL32.DLL", "RtlUnwind",              reinterpret_cast<void*>(&hle_rtl_unwind_ex_impl));
    register_function("KERNEL32.DLL", "RtlUnwindEx",            reinterpret_cast<void*>(&hle_rtl_unwind_ex_impl));
    register_function("KERNEL32.DLL", "RtlPcToFileHeader",      reinterpret_cast<void*>(&hle_rtl_pc_to_file_header));
    register_function("NTDLL.DLL",    "RtlLookupFunctionEntry", reinterpret_cast<void*>(&hle_rtl_lookup_function_entry));
    register_function("NTDLL.DLL",    "RtlVirtualUnwind",       reinterpret_cast<void*>(&hle_rtl_virtual_unwind));
    register_function("NTDLL.DLL",    "RtlCaptureContext",      reinterpret_cast<void*>(&hle_rtl_capture_context));
    register_function("NTDLL.DLL",    "RtlUnwind",              reinterpret_cast<void*>(&hle_rtl_unwind_ex_impl));
    register_function("NTDLL.DLL",    "RtlUnwindEx",            reinterpret_cast<void*>(&hle_rtl_unwind_ex_impl));
    register_function("NTDLL.DLL",    "RtlPcToFileHeader",      reinterpret_cast<void*>(&hle_rtl_pc_to_file_header));

    // KERNEL32 additions
    register_function("KERNEL32.DLL", "LCIDToLocaleName",       reinterpret_cast<void*>(&hle_lcid_to_locale_name));
    register_function("KERNEL32.DLL", "HeapSize",               reinterpret_cast<void*>(&hle_heap_size));
    register_function("KERNEL32.DLL", "K32GetPerformanceInfo",  reinterpret_cast<void*>(&hle_k32_get_performance_info));
    register_function("KERNEL32.DLL", "TerminateProcess",       reinterpret_cast<void*>(&hle_terminate_process));
    register_function("KERNEL32.DLL", "GetLargePageMinimum",    reinterpret_cast<void*>(&hle_get_large_page_minimum));
    register_function("KERNEL32.DLL", "AttachConsole",          reinterpret_cast<void*>(&hle_attach_console));
    register_function("KERNEL32.DLL", "GetConsoleMode",         reinterpret_cast<void*>(&hle_get_console_mode));
    register_function("KERNEL32.DLL", "SetConsoleMode",         reinterpret_cast<void*>(&hle_set_console_mode));
    register_function("KERNEL32.DLL", "GetConsoleOutputCP",     reinterpret_cast<void*>(&hle_get_console_output_cp));
    register_function("KERNEL32.DLL", "GetConsoleScreenBufferInfo", reinterpret_cast<void*>(&hle_get_console_screen_buffer_info));
    register_function("KERNEL32.DLL", "SetConsoleTextAttribute", reinterpret_cast<void*>(&hle_set_console_text_attribute));
    register_function("KERNEL32.DLL", "WriteConsoleW",          reinterpret_cast<void*>(&hle_write_console_w));
    register_function("KERNEL32.DLL", "ReadConsoleW",           reinterpret_cast<void*>(&hle_read_console_w));

    // USER32 additions
    register_function("USER32.DLL", "GetMonitorInfoW",          reinterpret_cast<void*>(&hle_get_monitor_info_w));
    register_function("USER32.DLL", "EnumDisplaySettingsW",     reinterpret_cast<void*>(&hle_enum_display_settings_w));
    register_function("USER32.DLL", "IsWindow",                 reinterpret_cast<void*>(&hle_is_window));
    register_function("USER32.DLL", "IsWindowVisible",          reinterpret_cast<void*>(&hle_is_window_visible));
    register_function("USER32.DLL", "IsZoomed",                 reinterpret_cast<void*>(&hle_is_zoomed));
    register_function("USER32.DLL", "IsIconic",                 reinterpret_cast<void*>(&hle_is_iconic));
    register_function("USER32.DLL", "MoveWindow",               reinterpret_cast<void*>(&hle_move_window));
    register_function("USER32.DLL", "FlashWindowEx",            reinterpret_cast<void*>(&hle_flash_window_ex));
    register_function("USER32.DLL", "SetFocus",                 reinterpret_cast<void*>(&hle_set_focus));
    register_function("USER32.DLL", "AllowSetForegroundWindow", reinterpret_cast<void*>(&hle_allow_set_foreground_window));
    register_function("USER32.DLL", "OpenClipboard",            reinterpret_cast<void*>(&hle_open_clipboard));
    register_function("USER32.DLL", "CloseClipboard",           reinterpret_cast<void*>(&hle_close_clipboard));
    register_function("USER32.DLL", "EmptyClipboard",           reinterpret_cast<void*>(&hle_empty_clipboard));
    register_function("USER32.DLL", "GetClipboardData",         reinterpret_cast<void*>(&hle_get_clipboard_data));
    register_function("USER32.DLL", "SetClipboardData",         reinterpret_cast<void*>(&hle_set_clipboard_data));
    register_function("USER32.DLL", "IsClipboardFormatAvailable", reinterpret_cast<void*>(&hle_is_clipboard_format_available));
    register_function("USER32.DLL", "TrackMouseEvent",          reinterpret_cast<void*>(&hle_track_mouse_event));
    register_function("USER32.DLL", "GetMessageExtraInfo",      reinterpret_cast<void*>(&hle_get_message_extra_info));
    register_function("USER32.DLL", "CallWindowProcW",          reinterpret_cast<void*>(&hle_call_window_proc_w));
    register_function("USER32.DLL", "GetRawInputDeviceList",    reinterpret_cast<void*>(&hle_get_raw_input_device_list));
    register_function("USER32.DLL", "GetRawInputDeviceInfoA",   reinterpret_cast<void*>(&hle_get_raw_input_device_info_a));
    register_function("USER32.DLL", "RegisterRawInputDevices",  reinterpret_cast<void*>(&hle_register_raw_input_devices));
    register_function("USER32.DLL", "GetRawInputData",          reinterpret_cast<void*>(&hle_get_raw_input_data));
    register_function("USER32.DLL", "CreateIconIndirect",       reinterpret_cast<void*>(&hle_create_icon_indirect));
    register_function("USER32.DLL", "CreateIconFromResource",   reinterpret_cast<void*>(&hle_create_icon_from_resource));
    register_function("USER32.DLL", "DestroyIcon",              reinterpret_cast<void*>(&hle_destroy_icon));
    register_function("USER32.DLL", "SetWindowsHookExA",        reinterpret_cast<void*>(&hle_set_windows_hook_ex_a));
    register_function("USER32.DLL", "UnhookWindowsHookEx",      reinterpret_cast<void*>(&hle_unhook_windows_hook_ex));
    register_function("USER32.DLL", "CallNextHookEx",           reinterpret_cast<void*>(&hle_call_next_hook_ex));
    register_function("USER32.DLL", "ClipCursor",               reinterpret_cast<void*>(&hle_clip_cursor));
    register_function("USER32.DLL", "WindowFromPoint",          reinterpret_cast<void*>(&hle_window_from_point));
    register_function("USER32.DLL", "CreateCaret",              reinterpret_cast<void*>(&hle_create_caret));
    register_function("USER32.DLL", "DestroyCaret",             reinterpret_cast<void*>(&hle_destroy_caret));
    register_function("USER32.DLL", "SetCaretPos",              reinterpret_cast<void*>(&hle_set_caret_pos));
    register_function("USER32.DLL", "ToUnicodeEx",              reinterpret_cast<void*>(&hle_to_unicode_ex));
    register_function("USER32.DLL", "MapVirtualKeyExA",         reinterpret_cast<void*>(&hle_map_virtual_key_ex_a));
    register_function("USER32.DLL", "GetKeyboardLayout",        reinterpret_cast<void*>(&hle_get_keyboard_layout));
    register_function("USER32.DLL", "GetKeyboardLayoutList",    reinterpret_cast<void*>(&hle_get_keyboard_layout_list));
    register_function("USER32.DLL", "ActivateKeyboardLayout",   reinterpret_cast<void*>(&hle_activate_keyboard_layout));
    register_function("USER32.DLL", "GetUpdateRect",            reinterpret_cast<void*>(&hle_get_update_rect));
    register_function("USER32.DLL", "SetWindowRgn",             reinterpret_cast<void*>(&hle_set_window_rgn));
    register_function("USER32.DLL", "RegisterTouchWindow",      reinterpret_cast<void*>(&hle_register_touch_window));
    register_function("USER32.DLL", "CloseTouchInputHandle",    reinterpret_cast<void*>(&hle_close_touch_input_handle));
    register_function("USER32.DLL", "GetTouchInputInfo",        reinterpret_cast<void*>(&hle_get_touch_input_info));

    // GDI32 additions
    register_function("GDI32.DLL", "CreateRectRgn",             reinterpret_cast<void*>(&hle_create_rect_rgn));
    register_function("GDI32.DLL", "CreatePolygonRgn",          reinterpret_cast<void*>(&hle_create_polygon_rgn));
    register_function("GDI32.DLL", "CreateBitmap",              reinterpret_cast<void*>(&hle_create_bitmap));
    register_function("GDI32.DLL", "CreateDIBSection",          reinterpret_cast<void*>(&hle_create_dib_section));

    // SHLWAPI
    register_function("SHLWAPI.DLL", "PathFileExistsW",         reinterpret_cast<void*>(&hle_path_file_exists_w));
    register_function("shlwapi.dll", "PathFileExistsW",         reinterpret_cast<void*>(&hle_path_file_exists_w));

    // OLE & OLEAUT32
    register_function("ole32.dll", "PropVariantClear",          reinterpret_cast<void*>(&hle_prop_variant_clear));
    register_function("OLEAUT32.dll", "SysAllocString",         reinterpret_cast<void*>(&hle_sys_alloc_string));
    register_function("OLEAUT32.dll", "SysAllocStringLen",      reinterpret_cast<void*>(&hle_sys_alloc_string_len));
    register_function("OLEAUT32.dll", "SysFreeString",          reinterpret_cast<void*>(&hle_sys_free_string));
    register_function("OLEAUT32.dll", "SysStringLen",           reinterpret_cast<void*>(&hle_sys_string_len));
    register_function("OLEAUT32.dll", "2",                      reinterpret_cast<void*>(&hle_sys_alloc_string));
    register_function("OLEAUT32.dll", "6",                      reinterpret_cast<void*>(&hle_sys_free_string));
    register_function("OLEAUT32.dll", "8",                      reinterpret_cast<void*>(&hle_sys_alloc_string_len));

    // WINMM
    register_function("WINMM.dll", "midiInGetNumDevs",          reinterpret_cast<void*>(&hle_midi_in_get_num_devs));
    register_function("WINMM.dll", "midiInOpen",                reinterpret_cast<void*>(&hle_midi_in_open));
    register_function("WINMM.dll", "midiInClose",               reinterpret_cast<void*>(&hle_midi_in_close));
    register_function("WINMM.dll", "midiInStart",               reinterpret_cast<void*>(&hle_midi_in_start));
    register_function("WINMM.dll", "midiInStop",                reinterpret_cast<void*>(&hle_midi_in_stop));
    register_function("WINMM.dll", "midiInGetID",               reinterpret_cast<void*>(&hle_midi_in_get_id));
    register_function("WINMM.dll", "midiInGetDevCapsA",         reinterpret_cast<void*>(&hle_midi_in_get_dev_caps_a));
    register_function("WINMM.dll", "midiInGetErrorTextA",       reinterpret_cast<void*>(&hle_midi_in_get_error_text_a));

    // IPHLPAPI
    register_function("IPHLPAPI.DLL", "GetAdaptersAddresses",   reinterpret_cast<void*>(&hle_get_adapters_addresses));
    register_function("IPHLPAPI.DLL", "GetBestInterfaceEx",     reinterpret_cast<void*>(&hle_get_best_interface_ex));

    // WSOCK32 ordinals
    register_function("WSOCK32.dll", "1",                       reinterpret_cast<void*>(&hle_accept));
    register_function("WSOCK32.dll", "2",                       reinterpret_cast<void*>(&hle_bind));
    register_function("WSOCK32.dll", "3",                       reinterpret_cast<void*>(&hle_closesocket));
    register_function("WSOCK32.dll", "4",                       reinterpret_cast<void*>(&hle_connect));
    register_function("WSOCK32.dll", "6",                       reinterpret_cast<void*>(&hle_getsockname));
    register_function("WSOCK32.dll", "8",                       reinterpret_cast<void*>(&hle_htonl));
    register_function("WSOCK32.dll", "9",                       reinterpret_cast<void*>(&hle_htons));
    register_function("WSOCK32.dll", "10",                      reinterpret_cast<void*>(&hle_inet_addr));
    register_function("WSOCK32.dll", "11",                      reinterpret_cast<void*>(&hle_inet_ntoa));
    register_function("WSOCK32.dll", "12",                      reinterpret_cast<void*>(&hle_listen));
    register_function("WSOCK32.dll", "13",                      reinterpret_cast<void*>(&hle_ntohl));
    register_function("WSOCK32.dll", "14",                      reinterpret_cast<void*>(&hle_ntohs));
    register_function("WSOCK32.dll", "15",                      reinterpret_cast<void*>(&hle_recv));
    register_function("WSOCK32.dll", "16",                      reinterpret_cast<void*>(&hle_recv));
    register_function("WSOCK32.dll", "17",                      reinterpret_cast<void*>(&hle_recv));
    register_function("WSOCK32.dll", "18",                      reinterpret_cast<void*>(&hle_select));
    register_function("WSOCK32.dll", "19",                      reinterpret_cast<void*>(&hle_send));
    register_function("WSOCK32.dll", "20",                      reinterpret_cast<void*>(&hle_send));
    register_function("WSOCK32.dll", "21",                      reinterpret_cast<void*>(&hle_setsockopt));
    register_function("WSOCK32.dll", "22",                      reinterpret_cast<void*>(&hle_shutdown));
    register_function("WSOCK32.dll", "23",                      reinterpret_cast<void*>(&hle_socket));
    register_function("WSOCK32.dll", "111",                     reinterpret_cast<void*>(&hle_wsa_get_last_error));
    register_function("WSOCK32.dll", "115",                     reinterpret_cast<void*>(&hle_wsa_startup));
    register_function("WSOCK32.dll", "116",                     reinterpret_cast<void*>(&hle_wsa_cleanup));
    register_function("WSOCK32.dll", "151",                     reinterpret_cast<void*>(&hle_wsa_startup));

    // DXGI.DLL & D3D11.DLL & DINPUT8.DLL
    register_function("DXGI.DLL", "CreateDXGIFactory",    reinterpret_cast<void*>(&hle_create_dxgi_factory));
    register_function("DXGI.DLL", "CreateDXGIFactory1",   reinterpret_cast<void*>(&hle_create_dxgi_factory));
    register_function("DXGI.DLL", "CreateDXGIFactory2",   reinterpret_cast<void*>(&hle_create_dxgi_factory));
    register_function("D3D11.DLL", "D3D11CreateDevice",   reinterpret_cast<void*>(&hle_d3d11_create_device));
    register_function("D3D11.DLL", "D3D11CreateDeviceAndSwapChain", reinterpret_cast<void*>(&hle_d3d11_create_device_and_swapchain));
    register_function("dsound.dll", "DirectSoundCreate",  reinterpret_cast<void*>(&hle_direct_sound_create));
    register_function("dsound.dll", "DirectSoundCreate8", reinterpret_cast<void*>(&hle_direct_sound_create8));
    register_function("dsound.dll", "DirectSoundEnumerateA", reinterpret_cast<void*>(&hle_direct_sound_enumerate_a));
    register_function("DINPUT8.DLL", "DirectInput8Create", reinterpret_cast<void*>(&hle_direct_input8_create));
    register_function("dinput.dll", "DirectInputCreateA", reinterpret_cast<void*>(&hle_direct_input_create_a));
    register_function("dinput.dll", "DirectInputCreateW", reinterpret_cast<void*>(&hle_direct_input_create_a));
    register_function("dinput.dll", "DirectInputCreateEx", reinterpret_cast<void*>(&hle_direct_input_create_a));
    // Synchronization additions (WaitOnAddress / WakeByAddress)
    register_function("KERNEL32.DLL", "WaitOnAddress",          reinterpret_cast<void*>(&hle_wait_on_address));
    register_function("KERNEL32.DLL", "WakeByAddressAll",       reinterpret_cast<void*>(&hle_wake_by_address_all));
    register_function("KERNEL32.DLL", "WakeByAddressSingle",    reinterpret_cast<void*>(&hle_wake_by_address_single));

    // WS2_32 / WSOCK32 additions
    register_function("WS2_32.dll", "sendto",                   reinterpret_cast<void*>(&hle_sendto));
    register_function("WS2_32.dll", "recvfrom",                 reinterpret_cast<void*>(&hle_recvfrom));
    register_function("WSOCK32.dll", "sendto",                  reinterpret_cast<void*>(&hle_sendto));
    register_function("WSOCK32.dll", "recvfrom",                reinterpret_cast<void*>(&hle_recvfrom));

    // UCRT / MSVCRT additions
    register_function("MSVCRT.DLL", "getenv",                   reinterpret_cast<void*>(&hle_getenv));
    register_function("MSVCRT.DLL", "_putenv_s",                reinterpret_cast<void*>(&hle_putenv_s));
    register_function("MSVCRT.DLL", "_wgetcwd",                 reinterpret_cast<void*>(&hle_wgetcwd));
    register_function("MSVCRT.DLL", "__p__environ",             reinterpret_cast<void*>(&hle_p_environ));
    register_function("MSVCRT.DLL", "_time64",                  reinterpret_cast<void*>(&hle_time64));
    register_function("MSVCRT.DLL", "_gmtime64",                reinterpret_cast<void*>(&hle_gmtime64));
    register_function("MSVCRT.DLL", "_gmtime64_s",              reinterpret_cast<void*>(&hle_gmtime64_s));
    register_function("MSVCRT.DLL", "_strftime_l",              reinterpret_cast<void*>(&hle_strftime_l));
    register_function("MSVCRT.DLL", "_tzset",                   reinterpret_cast<void*>(&hle_tzset));
    register_function("MSVCRT.DLL", "__daylight",               reinterpret_cast<void*>(&hle_daylight));
    register_function("MSVCRT.DLL", "__timezone",               reinterpret_cast<void*>(&hle_timezone));
    register_function("MSVCRT.DLL", "__tzname",                 reinterpret_cast<void*>(&hle_tzname));
    register_function("MSVCRT.DLL", "qsort",                    reinterpret_cast<void*>(&hle_qsort));
    register_function("MSVCRT.DLL", "bsearch",                  reinterpret_cast<void*>(&hle_bsearch));
    register_function("MSVCRT.DLL", "rand",                     reinterpret_cast<void*>(&hle_rand));
    register_function("MSVCRT.DLL", "srand",                    reinterpret_cast<void*>(&hle_srand));
    register_function("MSVCRT.DLL", "_mbtowc_l",                reinterpret_cast<void*>(&hle_mbtowc_l));
    register_function("MSVCRT.DLL", "setlocale",                reinterpret_cast<void*>(&hle_setlocale));
    register_function("MSVCRT.DLL", "_create_locale",           reinterpret_cast<void*>(&hle_create_locale));
    register_function("MSVCRT.DLL", "_free_locale",             reinterpret_cast<void*>(&hle_free_locale));
    register_function("MSVCRT.DLL", "strtod",                   reinterpret_cast<void*>(&hle_strtod));
    register_function("MSVCRT.DLL", "strtof",                   reinterpret_cast<void*>(&hle_strtof));
    register_function("MSVCRT.DLL", "strtol",                   reinterpret_cast<void*>(&hle_strtol));
    register_function("MSVCRT.DLL", "strtoll",                  reinterpret_cast<void*>(&hle_strtoll));
    register_function("MSVCRT.DLL", "strtoul",                  reinterpret_cast<void*>(&hle_strtoul));
    register_function("MSVCRT.DLL", "strtoull",                 reinterpret_cast<void*>(&hle_strtoull));
    register_function("MSVCRT.DLL", "wcstod",                   reinterpret_cast<void*>(&hle_wcstod));
    register_function("MSVCRT.DLL", "wcstol",                   reinterpret_cast<void*>(&hle_wcstol));
    register_function("MSVCRT.DLL", "wcstoll",                  reinterpret_cast<void*>(&hle_wcstoll));
    register_function("MSVCRT.DLL", "wcstoul",                  reinterpret_cast<void*>(&hle_wcstoul));
    register_function("MSVCRT.DLL", "wcstoull",                 reinterpret_cast<void*>(&hle_wcstoull));
    register_function("MSVCRT.DLL", "_strtod_l",                reinterpret_cast<void*>(&hle_strtod_l));
    register_function("MSVCRT.DLL", "_strtoi64_l",              reinterpret_cast<void*>(&hle_strtoi64_l));
    register_function("MSVCRT.DLL", "_strtoui64_l",             reinterpret_cast<void*>(&hle_strtoui64_l));
    register_function("MSVCRT.DLL", "_itoa_s",                  reinterpret_cast<void*>(&hle_itoa_s));
    register_function("MSVCRT.DLL", "wctob",                    reinterpret_cast<void*>(&hle_wctob));
    register_function("MSVCRT.DLL", "btowc",                    reinterpret_cast<void*>(&hle_btowc));
    register_function("MSVCRT.DLL", "mbrtowc",                  reinterpret_cast<void*>(&hle_mbrtowc));
    register_function("MSVCRT.DLL", "mbsrtowcs",                reinterpret_cast<void*>(&hle_mbsrtowcs));
    register_function("MSVCRT.DLL", "_aligned_malloc",          reinterpret_cast<void*>(&hle_aligned_malloc));
    register_function("MSVCRT.DLL", "_aligned_free",            reinterpret_cast<void*>(&hle_aligned_free));
    register_function("MSVCRT.DLL", "_set_new_mode",            reinterpret_cast<void*>(&hle_set_new_mode));
    register_function("MSVCRT.DLL", "__pctype_func",            reinterpret_cast<void*>(&hle_pctype_func));
    register_function("MSVCRT.DLL", "_configthreadlocale",      reinterpret_cast<void*>(&hle_configthreadlocale));
    register_function("MSVCRT.DLL", "memchr",                   reinterpret_cast<void*>(&hle_memchr));
    register_function("MSVCRT.DLL", "memcmp",                   reinterpret_cast<void*>(&hle_memcmp));
    register_function("MSVCRT.DLL", "strchr",                   reinterpret_cast<void*>(&hle_strchr));
    register_function("MSVCRT.DLL", "strrchr",                  reinterpret_cast<void*>(&hle_strrchr));
    register_function("MSVCRT.DLL", "strstr",                   reinterpret_cast<void*>(&hle_strstr));

    // Math functions
    register_function("MSVCRT.DLL", "sin",                      reinterpret_cast<void*>(&hle_sin));
    register_function("MSVCRT.DLL", "sinf",                     reinterpret_cast<void*>(&hle_sinf));
    register_function("MSVCRT.DLL", "cos",                      reinterpret_cast<void*>(&hle_cos));
    register_function("MSVCRT.DLL", "cosf",                     reinterpret_cast<void*>(&hle_cosf));
    register_function("MSVCRT.DLL", "tan",                      reinterpret_cast<void*>(&hle_tan));
    register_function("MSVCRT.DLL", "tanf",                     reinterpret_cast<void*>(&hle_tanf));
    register_function("MSVCRT.DLL", "sinh",                     reinterpret_cast<void*>(&hle_sinh));
    register_function("MSVCRT.DLL", "cosh",                     reinterpret_cast<void*>(&hle_cosh));
    register_function("MSVCRT.DLL", "tanh",                     reinterpret_cast<void*>(&hle_tanh));
    register_function("MSVCRT.DLL", "tanhf",                    reinterpret_cast<void*>(&hle_tanhf));
    register_function("MSVCRT.DLL", "sqrt",                     reinterpret_cast<void*>(&hle_sqrt));
    register_function("MSVCRT.DLL", "sqrtf",                    reinterpret_cast<void*>(&hle_sqrtf));
    register_function("MSVCRT.DLL", "pow",                      reinterpret_cast<void*>(&hle_pow));
    register_function("MSVCRT.DLL", "powf",                     reinterpret_cast<void*>(&hle_powf));
    register_function("MSVCRT.DLL", "log",                      reinterpret_cast<void*>(&hle_log));
    register_function("MSVCRT.DLL", "logf",                     reinterpret_cast<void*>(&hle_logf));
    register_function("MSVCRT.DLL", "exp",                      reinterpret_cast<void*>(&hle_exp));
    register_function("MSVCRT.DLL", "expf",                     reinterpret_cast<void*>(&hle_expf));
    register_function("MSVCRT.DLL", "floor",                    reinterpret_cast<void*>(&hle_floor));
    register_function("MSVCRT.DLL", "floorf",                   reinterpret_cast<void*>(&hle_floorf));
    register_function("MSVCRT.DLL", "ceil",                     reinterpret_cast<void*>(&hle_ceil));
    register_function("MSVCRT.DLL", "ceilf",                    reinterpret_cast<void*>(&hle_ceilf));
    register_function("MSVCRT.DLL", "fabs",                     reinterpret_cast<void*>(&hle_fabs));
    register_function("MSVCRT.DLL", "fabsf",                    reinterpret_cast<void*>(&hle_fabsf));
    register_function("MSVCRT.DLL", "atan",                     reinterpret_cast<void*>(&hle_atan));
    register_function("MSVCRT.DLL", "atanf",                    reinterpret_cast<void*>(&hle_atanf));
    register_function("MSVCRT.DLL", "atan2",                    reinterpret_cast<void*>(&hle_atan2));
    register_function("MSVCRT.DLL", "atan2f",                   reinterpret_cast<void*>(&hle_atan2f));
    register_function("MSVCRT.DLL", "asin",                     reinterpret_cast<void*>(&hle_asin));
    register_function("MSVCRT.DLL", "asinf",                    reinterpret_cast<void*>(&hle_asinf));
    register_function("MSVCRT.DLL", "acos",                     reinterpret_cast<void*>(&hle_acos));
    register_function("MSVCRT.DLL", "acosf",                    reinterpret_cast<void*>(&hle_acosf));
    register_function("MSVCRT.DLL", "fmod",                     reinterpret_cast<void*>(&hle_fmod));
    register_function("MSVCRT.DLL", "fmodf",                    reinterpret_cast<void*>(&hle_fmodf));
    register_function("MSVCRT.DLL", "modf",                     reinterpret_cast<void*>(&hle_modf));
    register_function("MSVCRT.DLL", "modff",                    reinterpret_cast<void*>(&hle_modff));
    register_function("MSVCRT.DLL", "remainder",                reinterpret_cast<void*>(&hle_remainder));
    register_function("MSVCRT.DLL", "remquo",                   reinterpret_cast<void*>(&hle_remquo));
    register_function("MSVCRT.DLL", "nextafter",                reinterpret_cast<void*>(&hle_nextafter));
    register_function("MSVCRT.DLL", "lrintf",                   reinterpret_cast<void*>(&hle_lrintf));
    register_function("MSVCRT.DLL", "log10",                    reinterpret_cast<void*>(&hle_log10));
    register_function("MSVCRT.DLL", "log10f",                   reinterpret_cast<void*>(&hle_log10f));
    register_function("MSVCRT.DLL", "log2",                     reinterpret_cast<void*>(&hle_log2));
    register_function("MSVCRT.DLL", "log2f",                    reinterpret_cast<void*>(&hle_log2f));
    register_function("MSVCRT.DLL", "hypot",                    reinterpret_cast<void*>(&hle_hypot));
    register_function("MSVCRT.DLL", "_hypot",                   reinterpret_cast<void*>(&hle_hypot));
    register_function("MSVCRT.DLL", "cbrt",                     reinterpret_cast<void*>(&hle_cbrt));
    register_function("MSVCRT.DLL", "cbrtf",                    reinterpret_cast<void*>(&hle_cbrtf));
    register_function("MSVCRT.DLL", "exp2",                     reinterpret_cast<void*>(&hle_exp2));
    register_function("MSVCRT.DLL", "exp2f",                    reinterpret_cast<void*>(&hle_exp2f));
    register_function("MSVCRT.DLL", "fma",                      reinterpret_cast<void*>(&hle_fma));
    register_function("MSVCRT.DLL", "fmaf",                     reinterpret_cast<void*>(&hle_fmaf));
    register_function("MSVCRT.DLL", "fmax",                     reinterpret_cast<void*>(&hle_fmax));
    register_function("MSVCRT.DLL", "fmaxf",                    reinterpret_cast<void*>(&hle_fmaxf));
    register_function("MSVCRT.DLL", "fmin",                     reinterpret_cast<void*>(&hle_fmin));
    register_function("MSVCRT.DLL", "fminf",                    reinterpret_cast<void*>(&hle_fminf));
    register_function("MSVCRT.DLL", "frexp",                    reinterpret_cast<void*>(&hle_frexp));
    register_function("MSVCRT.DLL", "ilogb",                    reinterpret_cast<void*>(&hle_ilogb));
    register_function("MSVCRT.DLL", "llrintf",                  reinterpret_cast<void*>(&hle_llrintf));
    register_function("MSVCRT.DLL", "acosh",                    reinterpret_cast<void*>(&hle_acosh));
    register_function("MSVCRT.DLL", "asinh",                    reinterpret_cast<void*>(&hle_asinh));
    register_function("MSVCRT.DLL", "atanh",                    reinterpret_cast<void*>(&hle_atanh));

    // Wide string functions
    register_function("MSVCRT.DLL", "wcslen",                   reinterpret_cast<void*>(&hle_wcslen));
    register_function("MSVCRT.DLL", "wcsnlen",                  reinterpret_cast<void*>(&hle_wcsnlen));
    register_function("MSVCRT.DLL", "wcscmp",                   reinterpret_cast<void*>(&hle_wcscmp));
    register_function("MSVCRT.DLL", "wcsncmp",                  reinterpret_cast<void*>(&hle_wcsncmp));
    register_function("MSVCRT.DLL", "wcscpy",                   reinterpret_cast<void*>(&hle_wcscpy));
    register_function("MSVCRT.DLL", "wcsncpy",                  reinterpret_cast<void*>(&hle_wcsncpy));
    register_function("MSVCRT.DLL", "wcscpy_s",                 reinterpret_cast<void*>(&hle_wcscpy_s));
    register_function("MSVCRT.DLL", "wcsstr",                   reinterpret_cast<void*>(&hle_wcsstr));
    register_function("MSVCRT.DLL", "wcschr",                   reinterpret_cast<void*>(&hle_wcschr));
    register_function("MSVCRT.DLL", "wcsrchr",                  reinterpret_cast<void*>(&hle_wcsrchr));
    register_function("MSVCRT.DLL", "wcrtomb",                  reinterpret_cast<void*>(&hle_wcrtomb));
    register_function("MSVCRT.DLL", "wcrtomb_s",                reinterpret_cast<void*>(&hle_wcrtomb_s));
    register_function("MSVCRT.DLL", "isalnum",                  reinterpret_cast<void*>(&hle_isalnum));
    register_function("MSVCRT.DLL", "isalpha",                  reinterpret_cast<void*>(&hle_isalpha));
    register_function("MSVCRT.DLL", "ispunct",                  reinterpret_cast<void*>(&hle_ispunct));
    register_function("MSVCRT.DLL", "isspace",                  reinterpret_cast<void*>(&hle_isspace));
    register_function("MSVCRT.DLL", "isxdigit",                 reinterpret_cast<void*>(&hle_isxdigit));
    register_function("MSVCRT.DLL", "tolower",                  reinterpret_cast<void*>(&hle_tolower));
    register_function("MSVCRT.DLL", "toupper",                  reinterpret_cast<void*>(&hle_toupper));
    register_function("MSVCRT.DLL", "_tolower_l",               reinterpret_cast<void*>(&hle_tolower_l));
    register_function("MSVCRT.DLL", "_toupper_l",               reinterpret_cast<void*>(&hle_toupper_l));
    register_function("MSVCRT.DLL", "_towlower_l",              reinterpret_cast<void*>(&hle_towlower_l));
    register_function("MSVCRT.DLL", "_towupper_l",              reinterpret_cast<void*>(&hle_towupper_l));
    register_function("MSVCRT.DLL", "_iswxdigit_l",             reinterpret_cast<void*>(&hle_iswxdigit_l));
    register_function("MSVCRT.DLL", "_strdup",                  reinterpret_cast<void*>(&hle_strdup));
    register_function("MSVCRT.DLL", "_stricmp",                 reinterpret_cast<void*>(&hle_stricmp));
    register_function("MSVCRT.DLL", "_memicmp",                 reinterpret_cast<void*>(&hle_memicmp));
    register_function("MSVCRT.DLL", "_strcoll_l",               reinterpret_cast<void*>(&hle_strcoll_l));
    register_function("MSVCRT.DLL", "_wcscoll_l",               reinterpret_cast<void*>(&hle_wcscoll_l));
    register_function("MSVCRT.DLL", "_strxfrm_l",               reinterpret_cast<void*>(&hle_strxfrm_l));
    register_function("MSVCRT.DLL", "_wcsxfrm_l",               reinterpret_cast<void*>(&hle_wcsxfrm_l));
    register_function("MSVCRT.DLL", "strcpy_s",                 reinterpret_cast<void*>(&hle_strcpy_s));
    register_function("MSVCRT.DLL", "strcat_s",                 reinterpret_cast<void*>(&hle_strcat_s));
    register_function("MSVCRT.DLL", "strncat",                  reinterpret_cast<void*>(&hle_strncat));
    register_function("MSVCRT.DLL", "strcspn",                  reinterpret_cast<void*>(&hle_strcspn));
    register_function("MSVCRT.DLL", "strpbrk",                  reinterpret_cast<void*>(&hle_strpbrk));
    register_function("MSVCRT.DLL", "strnlen",                  reinterpret_cast<void*>(&hle_strnlen));
    register_function("MSVCRT.DLL", "mbrlen",                   reinterpret_cast<void*>(&hle_mbrlen));

    // Runtime & Filesystem
    register_function("MSVCRT.DLL", "_configure_narrow_argv",   reinterpret_cast<void*>(&hle_configure_narrow_argv));
    register_function("MSVCRT.DLL", "_initialize_narrow_environment", reinterpret_cast<void*>(&hle_initialize_narrow_environment));
    register_function("MSVCRT.DLL", "_initterm_e",              reinterpret_cast<void*>(&hle_initterm_e));
    register_function("MSVCRT.DLL", "_crt_atexit",              reinterpret_cast<void*>(&hle_crt_atexit));
    register_function("MSVCRT.DLL", "_register_thread_local_exe_atexit_callback", reinterpret_cast<void*>(&hle_register_thread_local_exe_atexit_callback));
    register_function("MSVCRT.DLL", "_set_error_mode",          reinterpret_cast<void*>(&hle_set_error_mode));
    register_function("MSVCRT.DLL", "_set_invalid_parameter_handler", reinterpret_cast<void*>(&hle_set_invalid_parameter_handler));
    register_function("MSVCRT.DLL", "_endthreadex",             reinterpret_cast<void*>(&hle_endthreadex));
    register_function("MSVCRT.DLL", "_getpid",                  reinterpret_cast<void*>(&hle_getpid));
    register_function("MSVCRT.DLL", "perror",                   reinterpret_cast<void*>(&hle_perror));
    register_function("MSVCRT.DLL", "strerror_s",               reinterpret_cast<void*>(&hle_strerror_s));
    register_function("MSVCRT.DLL", "_fstat64",                 reinterpret_cast<void*>(&hle_fstat64));
    register_function("MSVCRT.DLL", "_lock_file",               reinterpret_cast<void*>(&hle_lock_file));
    register_function("MSVCRT.DLL", "_unlock_file",             reinterpret_cast<void*>(&hle_unlock_file));
    register_function("MSVCRT.DLL", "_wchdir",                  reinterpret_cast<void*>(&hle_wchdir));
    register_function("MSVCRT.DLL", "remove",                   reinterpret_cast<void*>(&hle_remove));
    register_function("MSVCRT.DLL", "__p___argc",               reinterpret_cast<void*>(&hle_p___argc));
    register_function("MSVCRT.DLL", "__p___argv",               reinterpret_cast<void*>(&hle_p___argv));
    register_function("MSVCRT.DLL", "__sys_nerr",               reinterpret_cast<void*>(&hle_sys_nerr));
    register_function("MSVCRT.DLL", "_assert",                  reinterpret_cast<void*>(&hle_assert));
    register_function("MSVCRT.DLL", "_beginthreadex",           reinterpret_cast<void*>(&hle_beginthreadex));

    // stdio
    register_function("MSVCRT.DLL", "fopen",                    reinterpret_cast<void*>(&hle_fopen));
    register_function("MSVCRT.DLL", "_wfopen",                  reinterpret_cast<void*>(&hle_wfopen));
    register_function("MSVCRT.DLL", "_wfopen_s",                reinterpret_cast<void*>(&hle_wfopen_s));
    register_function("MSVCRT.DLL", "_wfsopen",                 reinterpret_cast<void*>(&hle_wfsopen));
    register_function("MSVCRT.DLL", "fclose",                   reinterpret_cast<void*>(&hle_fclose));
    register_function("MSVCRT.DLL", "fread",                    reinterpret_cast<void*>(&hle_fread));
    register_function("MSVCRT.DLL", "fseek",                    reinterpret_cast<void*>(&hle_fseek));
    register_function("MSVCRT.DLL", "ftell",                    reinterpret_cast<void*>(&hle_ftell));
    register_function("MSVCRT.DLL", "fsetpos",                  reinterpret_cast<void*>(&hle_fsetpos));
    register_function("MSVCRT.DLL", "fgetpos",                  reinterpret_cast<void*>(&hle_fgetpos));
    register_function("MSVCRT.DLL", "feof",                     reinterpret_cast<void*>(&hle_feof));
    register_function("MSVCRT.DLL", "ferror",                   reinterpret_cast<void*>(&hle_ferror));
    register_function("MSVCRT.DLL", "fgets",                    reinterpret_cast<void*>(&hle_fgets));
    register_function("MSVCRT.DLL", "fgetwc",                   reinterpret_cast<void*>(&hle_fgetwc));
    register_function("MSVCRT.DLL", "fputwc",                   reinterpret_cast<void*>(&hle_fputwc));
    register_function("MSVCRT.DLL", "getc",                     reinterpret_cast<void*>(&hle_getc));
    register_function("MSVCRT.DLL", "putchar",                  reinterpret_cast<void*>(&hle_putchar));
    register_function("MSVCRT.DLL", "rewind",                   reinterpret_cast<void*>(&hle_rewind));
    register_function("MSVCRT.DLL", "setbuf",                   reinterpret_cast<void*>(&hle_setbuf));
    register_function("MSVCRT.DLL", "setvbuf",                  reinterpret_cast<void*>(&hle_setvbuf));
    register_function("MSVCRT.DLL", "ungetc",                   reinterpret_cast<void*>(&hle_ungetc));
    register_function("MSVCRT.DLL", "ungetwc",                  reinterpret_cast<void*>(&hle_ungetwc));
    register_function("MSVCRT.DLL", "freopen_s",                reinterpret_cast<void*>(&hle_freopen_s));
    register_function("MSVCRT.DLL", "__stdio_common_vfprintf",  reinterpret_cast<void*>(&hle_stdio_common_vfprintf));
    register_function("MSVCRT.DLL", "__stdio_common_vsprintf",  reinterpret_cast<void*>(&hle_stdio_common_vsprintf));
    register_function("MSVCRT.DLL", "__stdio_common_vsnprintf_s", reinterpret_cast<void*>(&hle_stdio_common_vsnprintf_s));
    register_function("MSVCRT.DLL", "__stdio_common_vsprintf_s", reinterpret_cast<void*>(&hle_stdio_common_vsprintf_s));
    register_function("MSVCRT.DLL", "__stdio_common_vswprintf", reinterpret_cast<void*>(&hle_stdio_common_vswprintf));
    register_function("MSVCRT.DLL", "__stdio_common_vfwprintf", reinterpret_cast<void*>(&hle_stdio_common_vfwprintf));
    register_function("MSVCRT.DLL", "__stdio_common_vfscanf",   reinterpret_cast<void*>(&hle_stdio_common_vfscanf));
    register_function("MSVCRT.DLL", "__stdio_common_vsscanf",   reinterpret_cast<void*>(&hle_stdio_common_vsscanf));
    register_function("MSVCRT.DLL", "__acrt_iob_func",          reinterpret_cast<void*>(&hle_acrt_iob_func));
    register_function("MSVCRT.DLL", "__p__commode",             reinterpret_cast<void*>(&hle_p__commode));
    register_function("MSVCRT.DLL", "__p__fmode",               reinterpret_cast<void*>(&hle_p__fmode));
    register_function("MSVCRT.DLL", "_fileno",                  reinterpret_cast<void*>(&hle_fileno));
    register_function("MSVCRT.DLL", "_fseeki64",                reinterpret_cast<void*>(&hle_fseeki64));
    register_function("MSVCRT.DLL", "_ftelli64",                reinterpret_cast<void*>(&hle_ftelli64));
    register_function("MSVCRT.DLL", "_get_osfhandle",           reinterpret_cast<void*>(&hle_get_osfhandle));
    register_function("MSVCRT.DLL", "_getmaxstdio",             reinterpret_cast<void*>(&hle_getmaxstdio));
    register_function("MSVCRT.DLL", "_setmaxstdio",             reinterpret_cast<void*>(&hle_setmaxstdio));
    register_function("MSVCRT.DLL", "_chsize_s",                reinterpret_cast<void*>(&hle_chsize_s));

    // UCRT (ucrtbase.dll) family. Modern Unity/Godot-Mono/.NET games import
    // the Universal CRT directly under ucrtbase.dll. Register the host-backed
    // CRT surface below; the PE resolver forwards ucrtbase imports here.
    register_function("UCRTBASE.DLL", "__p___wargv",                reinterpret_cast<void*>(&hle_p___wargv));
    register_function("UCRTBASE.DLL", "__p___argc",                 reinterpret_cast<void*>(&hle_p___argc));
    register_function("UCRTBASE.DLL", "__p___argv",                 reinterpret_cast<void*>(&hle_p___argv));
    register_function("UCRTBASE.DLL", "_configure_narrow_argv",     reinterpret_cast<void*>(&hle_configure_narrow_argv));
    register_function("UCRTBASE.DLL", "_crt_atexit",                reinterpret_cast<void*>(&hle_crt_atexit));
    register_function("UCRTBASE.DLL", "_initterm",                  reinterpret_cast<void*>(&hle_msvcrt__initterm));
    register_function("UCRTBASE.DLL", "_initterm_e",                reinterpret_cast<void*>(&hle_initterm_e));
    register_function("UCRTBASE.DLL", "_set_app_type",              reinterpret_cast<void*>(&hle_msvcrt__set_app_type));
    register_function("UCRTBASE.DLL", "exit",                       reinterpret_cast<void*>(&hle_msvcrt_exit));
    register_function("UCRTBASE.DLL", "_initialize_narrow_environment", reinterpret_cast<void*>(&hle_initialize_narrow_environment));
    register_function("UCRTBASE.DLL", "_configure_wide_argv",       reinterpret_cast<void*>(&hle_configure_wide_argv));
    register_function("UCRTBASE.DLL", "_get_initial_wide_environment", reinterpret_cast<void*>(&hle_get_initial_wide_environment));
    register_function("UCRTBASE.DLL", "_initialize_wide_environment", reinterpret_cast<void*>(&hle_initialize_wide_environment));
    register_function("UCRTBASE.DLL", "_get_initial_narrow_environment", reinterpret_cast<void*>(&hle_get_initial_narrow_environment));
    register_function("UCRTBASE.DLL", "_wcsicmp",                  reinterpret_cast<void*>(&hle_wcsicmp));
    register_function("UCRTBASE.DLL", "_wcsdup",                   reinterpret_cast<void*>(&hle_wcsdup));
    register_function("UCRTBASE.DLL", "wcscat",                    reinterpret_cast<void*>(&hle_wcscat));
    register_function("UCRTBASE.DLL", "_wcsnicmp",                 reinterpret_cast<void*>(&hle_wcsnicmp));
    register_function("UCRTBASE.DLL", "wcspbrk",                   reinterpret_cast<void*>(&hle_wcspbrk));
    register_function("UCRTBASE.DLL", "iswspace",                  reinterpret_cast<void*>(&hle_iswspace));
    register_function("UCRTBASE.DLL", "towupper",                  reinterpret_cast<void*>(&hle_towupper));
    register_function("UCRTBASE.DLL", "towlower",                  reinterpret_cast<void*>(&hle_towlower));
    register_function("UCRTBASE.DLL", "iswprint",                  reinterpret_cast<void*>(&hle_iswprint));
    register_function("UCRTBASE.DLL", "iswxdigit",                 reinterpret_cast<void*>(&hle_iswxdigit));
    register_function("UCRTBASE.DLL", "iswdigit",                  reinterpret_cast<void*>(&hle_iswdigit));
    register_function("UCRTBASE.DLL", "iswalnum",                  reinterpret_cast<void*>(&hle_iswalnum));
    register_function("UCRTBASE.DLL", "isprint",                   reinterpret_cast<void*>(&hle_isprint));
    register_function("UCRTBASE.DLL", "_wcsupr",                   reinterpret_cast<void*>(&hle_wcsupr));
    register_function("UCRTBASE.DLL", "_wcslwr",                   reinterpret_cast<void*>(&hle_wcslwr));
    register_function("UCRTBASE.DLL", "_wcsrev",                   reinterpret_cast<void*>(&hle_wcsrev));
    register_function("UCRTBASE.DLL", "wcstok",                    reinterpret_cast<void*>(&hle_wcstok));
    register_function("UCRTBASE.DLL", "wcstok_s",                  reinterpret_cast<void*>(&hle_wcstok_s));
    register_function("UCRTBASE.DLL", "wcscspn",                   reinterpret_cast<void*>(&hle_wcscspn));
    register_function("UCRTBASE.DLL", "wcsspn",                    reinterpret_cast<void*>(&hle_wcsspn));
    register_function("UCRTBASE.DLL", "wcsncat_s",                 reinterpret_cast<void*>(&hle_wcsncat_s));
    register_function("UCRTBASE.DLL", "wcsncpy_s",                 reinterpret_cast<void*>(&hle_wcsncpy_s));
    register_function("UCRTBASE.DLL", "_wsplitpath",               reinterpret_cast<void*>(&hle_wsplitpath));
    register_function("UCRTBASE.DLL", "_wtoi",                     reinterpret_cast<void*>(&hle_wtoi));
    register_function("UCRTBASE.DLL", "_wtol",                     reinterpret_cast<void*>(&hle_wtol));
    register_function("UCRTBASE.DLL", "_wcstoui64",                reinterpret_cast<void*>(&hle_wcstoui64));
    register_function("UCRTBASE.DLL", "_ui64tow",                  reinterpret_cast<void*>(&hle_ui64tow));
    register_function("UCRTBASE.DLL", "_wgetenv",                  reinterpret_cast<void*>(&hle_wgetenv));
    register_function("UCRTBASE.DLL", "_wperror",                  reinterpret_cast<void*>(&hle_wperror));
    register_function("UCRTBASE.DLL", "_time32",                   reinterpret_cast<void*>(&hle_time32));
    register_function("UCRTBASE.DLL", "_open",                     reinterpret_cast<void*>(&hle_open));
    register_function("UCRTBASE.DLL", "_wopen",                    reinterpret_cast<void*>(&hle_wopen));
    register_function("UCRTBASE.DLL", "_close",                    reinterpret_cast<void*>(&hle_close));
    register_function("UCRTBASE.DLL", "_read",                     reinterpret_cast<void*>(&hle_read));
    register_function("UCRTBASE.DLL", "_write",                    reinterpret_cast<void*>(&hle_write));
    register_function("UCRTBASE.DLL", "_lseek",                    reinterpret_cast<void*>(&hle_lseek));
    register_function("UCRTBASE.DLL", "_chsize",                   reinterpret_cast<void*>(&hle_chsize));
    register_function("UCRTBASE.DLL", "_fstat64i32",               reinterpret_cast<void*>(&hle_fstat64i32));
    register_function("UCRTBASE.DLL", "_setmode",                  reinterpret_cast<void*>(&hle_setmode));
    register_function("UCRTBASE.DLL", "_kbhit",                    reinterpret_cast<void*>(&hle_kbhit));
    register_function("UCRTBASE.DLL", "getc",                      reinterpret_cast<void*>(&hle_getc));
    register_function("UCRTBASE.DLL", "putc",                      reinterpret_cast<void*>(&hle_putc));
    register_function("UCRTBASE.DLL", "fgetws",                    reinterpret_cast<void*>(&hle_fgetws));
    register_function("UCRTBASE.DLL", "fputws",                    reinterpret_cast<void*>(&hle_fputws));
    register_function("UCRTBASE.DLL", "__stdio_common_vswscanf",   reinterpret_cast<void*>(&hle_stdio_common_vswscanf));
    register_function("UCRTBASE.DLL", "_pclose",                   reinterpret_cast<void*>(&hle_pclose));
    register_function("UCRTBASE.DLL", "_wpopen",                   reinterpret_cast<void*>(&hle_wpopen));

    // User32 additions
    register_function("USER32.DLL", "SetPropW",                 reinterpret_cast<void*>(&hle_set_prop_w));
    register_function("USER32.DLL", "SetMenuItemInfoW",         reinterpret_cast<void*>(&hle_set_menu_item_info_w));
    register_function("USER32.DLL", "SetWindowDisplayAffinity", reinterpret_cast<void*>(&hle_set_window_display_affinity));
    register_function("USER32.DLL", "TrackPopupMenuEx",         reinterpret_cast<void*>(&hle_track_popup_menu_ex));
    register_function("USER32.DLL", "UnregisterClassA",         reinterpret_cast<void*>(&hle_unregister_class_a));
    register_function("USER32.DLL", "UnregisterClassW",         reinterpret_cast<void*>(&hle_unregister_class_w));
    register_function("USER32.DLL", "UnregisterDeviceNotification", reinterpret_cast<void*>(&hle_unregister_device_notification));

    // isw helpers
    register_function("MSVCRT.DLL", "_isctype_l",               reinterpret_cast<void*>(&hle_isctype_l));
    register_function("MSVCRT.DLL", "_iswalpha_l",              reinterpret_cast<void*>(&hle_iswalpha_l));
    register_function("MSVCRT.DLL", "_iswcntrl_l",              reinterpret_cast<void*>(&hle_iswcntrl_l));
    register_function("MSVCRT.DLL", "_iswdigit_l",              reinterpret_cast<void*>(&hle_iswdigit_l));
    register_function("MSVCRT.DLL", "_iswlower_l",              reinterpret_cast<void*>(&hle_iswlower_l));
    register_function("MSVCRT.DLL", "_iswprint_l",              reinterpret_cast<void*>(&hle_iswprint_l));
    register_function("MSVCRT.DLL", "_iswpunct_l",              reinterpret_cast<void*>(&hle_iswpunct_l));
    register_function("MSVCRT.DLL", "_iswspace_l",              reinterpret_cast<void*>(&hle_iswspace_l));
    register_function("MSVCRT.DLL", "_iswupper_l",              reinterpret_cast<void*>(&hle_iswupper_l));

    // setjmp / longjmp
    register_function("MSVCRT.DLL", "__intrinsic_setjmpex",     reinterpret_cast<void*>(&hle_msvc_setjmp));
    register_function("MSVCRT.DLL", "_setjmp",                  reinterpret_cast<void*>(&hle_msvc_setjmp));
    register_function("MSVCRT.DLL", "setjmp",                   reinterpret_cast<void*>(&hle_msvc_setjmp));
    register_function("MSVCRT.DLL", "longjmp",                  reinterpret_cast<void*>(&hle_rtl_unwind_ex_impl));

    // ADVAPI32 additions
    register_function("ADVAPI32.DLL", "RegEnumKeyExW",          reinterpret_cast<void*>(&hle_reg_enum_key_ex_w));
    register_function("ADVAPI32.DLL", "RegOpenKeyW",            reinterpret_cast<void*>(&hle_reg_open_key_w));
    register_function("ADVAPI32.DLL", "RegQueryInfoKeyW",       reinterpret_cast<void*>(&hle_reg_query_info_key_w));

    // NTDLL additions
    register_function("NTDLL.DLL", "RtlNtStatusToDosError",     reinterpret_cast<void*>(&hle_rtl_nt_status_to_dos_error));
    register_function("NTDLL.DLL", "NtQueryInformationFile",    reinterpret_cast<void*>(&hle_nt_query_information_file));
    register_function("NTDLL.DLL", "NtWriteFile",               reinterpret_cast<void*>(&hle_nt_write_file));

    // WSOCK32 additions
    register_function("WSOCK32.DLL", "__WSAFDIsSet",            reinterpret_cast<void*>(&hle_wsa_fd_is_set));
    register_function("WSOCK32.DLL", "ioctlsocket",             reinterpret_cast<void*>(&hle_ioctlsocket));
    register_function("WS2_32.DLL", "__WSAFDIsSet",             reinterpret_cast<void*>(&hle_wsa_fd_is_set));
    register_function("WS2_32.DLL", "ioctlsocket",              reinterpret_cast<void*>(&hle_ioctlsocket));

    // UIAutomationCore additions
    register_function("UIAutomationCore.DLL", "UiaGetReservedNotSupportedValue", reinterpret_cast<void*>(&hle_uia_get_reserved_not_supported_value));
    register_function("UIAutomationCore.DLL", "UiaHostProviderFromHwnd",         reinterpret_cast<void*>(&hle_uia_host_provider_from_hwnd));
    register_function("UIAutomationCore.DLL", "UiaLookupId",                     reinterpret_cast<void*>(&hle_uia_lookup_id));
    register_function("UIAutomationCore.DLL", "UiaRaiseAutomationEvent",         reinterpret_cast<void*>(&hle_uia_raise_automation_event));
    register_function("UIAutomationCore.DLL", "UiaRaiseAutomationPropertyChangedEvent", reinterpret_cast<void*>(&hle_uia_raise_automation_property_changed_event));
    register_function("UIAutomationCore.DLL", "UiaReturnRawElementProvider",     reinterpret_cast<void*>(&hle_uia_return_raw_element_provider));

    // Register host-libc-backed CRT math forwards (real), then codegen stubs.
    register_crt_math_forwards(*this);
    register_codegen_stubs(*this);

    return {};
}

void Win32ApiHle::register_function(std::string_view dll_name, std::string_view function_name, void* func_ptr) {
    std::string upper_dll(dll_name);
    for (auto& c : upper_dll) c = static_cast<char>(std::toupper(c));
    export_table_[upper_dll][std::string(function_name)] = func_ptr;
}

void Win32ApiHle::register_stub(std::string_view dll_name, std::string_view function_name, void* func_ptr) {
    // Register into the same export table so the symbol resolves (imports don't
    // crash, GetProcAddress probes succeed), and record attribution so coverage
    // distinguishes codegen stubs from real implementations. Only fill a slot
    // that has no real implementation yet (real ones register first).
    std::string upper_dll(dll_name);
    for (auto& c : upper_dll) c = static_cast<char>(std::toupper(c));
    std::string name(function_name);
    auto& tbl = export_table_[upper_dll];
    if (tbl.find(name) != tbl.end()) return;   // never override a real impl
    tbl[name] = func_ptr;
    stub_exports_.insert(upper_dll + "!" + name);
}

void* Win32ApiHle::resolve_symbol(std::string_view dll_name, std::string_view function_name) {
    std::string upper_dll(dll_name);
    for (auto& c : upper_dll) c = static_cast<char>(std::toupper(c));
    std::string func_str(function_name);

    // 1. Direct DLL match
    auto dll_it = export_table_.find(upper_dll);
    if (dll_it != export_table_.end()) {
        auto func_it = dll_it->second.find(func_str);
        if (func_it != dll_it->second.end()) {
            return func_it->second;
        }
    }

    // 2. ApiSet schema & Core forwarding (api-ms-*, ext-ms-*, etc.)
    static const char* kCoreDlls[] = {
        "KERNEL32.DLL", "NTDLL.DLL", "USER32.DLL", "ADVAPI32.DLL",
        "OLE32.DLL", "GDI32.DLL", "SHELL32.DLL", "MSVCRT.DLL",
        "WS2_32.DLL", "WSOCK32.DLL", "WINMM.DLL", "OPENGL32.DLL", "D3D11.DLL",
        "DXGI.DLL", "DINPUT8.DLL", "XINPUT1_3.DLL", "IMM32.DLL",
        "CRYPT32.DLL", "BCRYPT.DLL", "DWMAPI.DLL", "AVRT.DLL",
        "UIAUTOMATIONCORE.DLL"
    };
    for (const char* core_dll : kCoreDlls) {
        auto cit = export_table_.find(core_dll);
        if (cit != export_table_.end()) {
            auto fit = cit->second.find(func_str);
            if (fit != cit->second.end()) {
                return fit->second;
            }
        }
    }

    // 3. Global fallback across any registered HLE table
    for (const auto& [d, funcs] : export_table_) {
        auto it = funcs.find(func_str);
        if (it != funcs.end()) {
            return it->second;
        }
    }

    return nullptr;
}

} // namespace papaya::win32
