#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/steam/steam_api_stub.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
#include <functional>
#include <memory>

namespace papaya::win32 {

using Win32SyscallFn = void* (*)();

class Win32ApiHle {
public:
    explicit Win32ApiHle(std::shared_ptr<steam::SteamApiStub> steam_stub = nullptr);
    ~Win32ApiHle() = default;

    Result<> initialize();

    // Resolves a function pointer for a DLL export
    void* resolve_symbol(std::string_view dll_name, std::string_view function_name);

    // Registers custom HLE function handler
    void register_function(std::string_view dll_name, std::string_view function_name, void* func_ptr);

    // Common Win32 HLE implementations
    static void* hle_virtual_alloc(void* lpAddress, size_t dwSize, u32 flAllocationType, u32 flProtect);
    static int   hle_virtual_free(void* lpAddress, size_t dwSize, u32 dwFreeType);
    static int   hle_virtual_protect(void* lpAddress, size_t dwSize, u32 flNewProtect, u32* lpflOldProtect);
    static void* hle_get_proc_address(void* hModule, const char* lpProcName);
    static void* hle_get_module_handle_a(const char* lpModuleName);
    static void* hle_load_library_a(const char* lpLibFileName);
    static u32   hle_get_current_thread_id();
    static u32   hle_get_current_process_id();
    static void  hle_exit_process(u32 uExitCode);
    static void  hle_sleep(u32 dwMilliseconds);
    static int   hle_query_performance_counter(s64* lpPerformanceCount);
    static int   hle_query_performance_frequency(s64* lpFrequency);
    static u32   hle_get_tick_count();
    static u64   hle_get_tick_count_64();
    static u32   hle_get_last_error();
    static void  hle_set_last_error(u32 dwErrCode);

private:
    std::shared_ptr<steam::SteamApiStub> steam_stub_;
    std::unordered_map<std::string, std::unordered_map<std::string, void*>> export_table_;
};

} // namespace papaya::win32
