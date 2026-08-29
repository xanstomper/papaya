#include "papaya/win32/win32_api_hle.hpp"
#include "papaya/common/logger.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <cstring>
#include <thread>
#include <chrono>

namespace papaya::win32 {

thread_local u32 g_last_error = 0;

void* Win32ApiHle::hle_virtual_alloc(void* lpAddress, size_t dwSize, u32 flAllocationType, u32 flProtect) {
    int prot = PROT_READ | PROT_WRITE;
    if (flProtect == 0x40 || flProtect == 0x20) prot |= PROT_EXEC; // PAGE_EXECUTE_READWRITE / PAGE_EXECUTE_READ

    void* ptr = mmap(lpAddress, dwSize, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        g_last_error = 8; // ERROR_NOT_ENOUGH_MEMORY
        return nullptr;
    }
    return ptr;
}

int Win32ApiHle::hle_virtual_free(void* lpAddress, size_t dwSize, u32 dwFreeType) {
    if (!lpAddress) return 0;
    munmap(lpAddress, dwSize);
    return 1;
}

int Win32ApiHle::hle_virtual_protect(void* lpAddress, size_t dwSize, u32 flNewProtect, u32* lpflOldProtect) {
    if (lpflOldProtect) *lpflOldProtect = 0x04; // PAGE_READWRITE
    int prot = PROT_READ | PROT_WRITE;
    if (flNewProtect == 0x40 || flNewProtect == 0x20) prot |= PROT_EXEC;
    return (mprotect(lpAddress, dwSize, prot) == 0) ? 1 : 0;
}

void* Win32ApiHle::hle_get_proc_address(void* hModule, const char* lpProcName) {
    return nullptr;
}

void* Win32ApiHle::hle_get_module_handle_a(const char* lpModuleName) {
    return reinterpret_cast<void*>(0x140000000); // Standard ImageBase mock
}

void* Win32ApiHle::hle_load_library_a(const char* lpLibFileName) {
    return reinterpret_cast<void*>(0x180000000);
}

u32 Win32ApiHle::hle_get_current_thread_id() {
    return static_cast<u32>(gettid());
}

u32 Win32ApiHle::hle_get_current_process_id() {
    return static_cast<u32>(getpid());
}

void Win32ApiHle::hle_exit_process(u32 uExitCode) {
    log::info("WIN32", "Game invoked ExitProcess({})", uExitCode);
    _exit(static_cast<int>(uExitCode));
}

void Win32ApiHle::hle_sleep(u32 dwMilliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(dwMilliseconds));
}

int Win32ApiHle::hle_query_performance_counter(s64* lpPerformanceCount) {
    if (!lpPerformanceCount) return 0;
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *lpPerformanceCount = static_cast<s64>(ts.tv_sec) * 1000000000LL + static_cast<s64>(ts.tv_nsec);
    return 1;
}

int Win32ApiHle::hle_query_performance_frequency(s64* lpFrequency) {
    if (!lpFrequency) return 0;
    *lpFrequency = 1000000000LL; // 1 GHz nanosecond resolution
    return 1;
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

Win32ApiHle::Win32ApiHle(std::shared_ptr<steam::SteamApiStub> steam_stub)
    : steam_stub_(steam_stub) {}

Result<> Win32ApiHle::initialize() {
    log::info("WIN32", "Initializing Papaya Native Win32 HLE Syscall Dispatcher");

    // Register KERNEL32.DLL exports
    register_function("KERNEL32.DLL", "VirtualAlloc", reinterpret_cast<void*>(&hle_virtual_alloc));
    register_function("KERNEL32.DLL", "VirtualFree", reinterpret_cast<void*>(&hle_virtual_free));
    register_function("KERNEL32.DLL", "VirtualProtect", reinterpret_cast<void*>(&hle_virtual_protect));
    register_function("KERNEL32.DLL", "GetProcAddress", reinterpret_cast<void*>(&hle_get_proc_address));
    register_function("KERNEL32.DLL", "GetModuleHandleA", reinterpret_cast<void*>(&hle_get_module_handle_a));
    register_function("KERNEL32.DLL", "GetModuleHandleW", reinterpret_cast<void*>(&hle_get_module_handle_a));
    register_function("KERNEL32.DLL", "LoadLibraryA", reinterpret_cast<void*>(&hle_load_library_a));
    register_function("KERNEL32.DLL", "LoadLibraryW", reinterpret_cast<void*>(&hle_load_library_a));
    register_function("KERNEL32.DLL", "GetCurrentThreadId", reinterpret_cast<void*>(&hle_get_current_thread_id));
    register_function("KERNEL32.DLL", "GetCurrentProcessId", reinterpret_cast<void*>(&hle_get_current_process_id));
    register_function("KERNEL32.DLL", "ExitProcess", reinterpret_cast<void*>(&hle_exit_process));
    register_function("KERNEL32.DLL", "Sleep", reinterpret_cast<void*>(&hle_sleep));
    register_function("KERNEL32.DLL", "QueryPerformanceCounter", reinterpret_cast<void*>(&hle_query_performance_counter));
    register_function("KERNEL32.DLL", "QueryPerformanceFrequency", reinterpret_cast<void*>(&hle_query_performance_frequency));
    register_function("KERNEL32.DLL", "GetTickCount", reinterpret_cast<void*>(&hle_get_tick_count));
    register_function("KERNEL32.DLL", "GetTickCount64", reinterpret_cast<void*>(&hle_get_tick_count_64));
    register_function("KERNEL32.DLL", "GetLastError", reinterpret_cast<void*>(&hle_get_last_error));
    register_function("KERNEL32.DLL", "SetLastError", reinterpret_cast<void*>(&hle_set_last_error));

    // Register NTDLL.DLL aliases
    register_function("NTDLL.DLL", "NtAllocateVirtualMemory", reinterpret_cast<void*>(&hle_virtual_alloc));
    register_function("NTDLL.DLL", "NtFreeVirtualMemory", reinterpret_cast<void*>(&hle_virtual_free));

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

    log::warn("WIN32", "Unresolved Win32 Import: {}!{}", dll_name, function_name);
    return nullptr;
}

} // namespace papaya::win32
