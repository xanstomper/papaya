#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/steam/steam_api_stub.hpp"
#include "papaya/input/virtual_xinput.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>

namespace papaya::win32 {

// Standard Win32 Handles and Types
using HANDLE = void*;
using HWND   = void*;
using HMODULE= void*;
using BOOL   = int;
using DWORD  = u32;
using QWORD  = u64;

constexpr BOOL TRUE_VAL  = 1;
constexpr BOOL FALSE_VAL = 0;

struct Win32SystemInfo {
    u16 wProcessorArchitecture{9}; // PROCESSOR_ARCHITECTURE_AMD64
    u16 wReserved{0};
    u32 dwPageSize{4096};
    void* lpMinimumApplicationAddress{reinterpret_cast<void*>(0x10000)};
    void* lpMaximumApplicationAddress{reinterpret_cast<void*>(0x7FFFFFFEFFFF)};
    u64 dwActiveProcessorMask{0xFF};
    u32 dwNumberOfProcessors{8};
    u32 dwProcessorType{8664};
    u32 dwAllocationGranularity{65536};
    u16 wProcessorLevel{6};
    u16 wProcessorRevision{0};
};

struct Win32CriticalSection {
    void* DebugInfo{nullptr};
    s32   LockCount{-1};
    s32   RecursionCount{0};
    HANDLE OwningThread{nullptr};
    HANDLE LockSemaphore{nullptr};
    u64   SpinCount{0};
};

struct Win32FileFindDataA {
    u32 dwFileAttributes{0};
    u32 ftCreationTimeLow{0};
    u32 ftCreationTimeHigh{0};
    u32 ftLastAccessTimeLow{0};
    u32 ftLastAccessTimeHigh{0};
    u32 ftLastWriteTimeLow{0};
    u32 ftLastWriteTimeHigh{0};
    u32 nFileSizeHigh{0};
    u32 nFileSizeLow{0};
    u32 dwReserved0{0};
    u32 dwReserved1{0};
    char cFileName[260]{0};
    char cAlternateFileName[14]{0};
};

class Win32ApiHle {
public:
    explicit Win32ApiHle(
        std::shared_ptr<steam::SteamApiStub> steam_stub = nullptr,
        std::shared_ptr<input::VirtualXInputManager> input_mgr = nullptr
    );
    ~Win32ApiHle() = default;

    Result<> initialize();

    // Symbol resolution for IAT patching
    void* resolve_symbol(std::string_view dll_name, std::string_view function_name);
    void register_function(std::string_view dll_name, std::string_view function_name, void* func_ptr);

    // KERNEL32 / NTDLL Emulation: Memory
    static PAPAYA_MS_ABI void* hle_virtual_alloc(void* lpAddress, size_t dwSize, u32 flAllocationType, u32 flProtect);
    static PAPAYA_MS_ABI BOOL  hle_virtual_free(void* lpAddress, size_t dwSize, u32 dwFreeType);
    static PAPAYA_MS_ABI BOOL  hle_virtual_protect(void* lpAddress, size_t dwSize, u32 flNewProtect, u32* lpflOldProtect);
    static PAPAYA_MS_ABI HANDLE hle_get_process_heap();
    static PAPAYA_MS_ABI void* hle_heap_alloc(HANDLE hHeap, u32 dwFlags, size_t dwBytes);
    static PAPAYA_MS_ABI BOOL  hle_heap_free(HANDLE hHeap, u32 dwFlags, void* lpMem);
    static PAPAYA_MS_ABI void* hle_heap_realloc(HANDLE hHeap, u32 dwFlags, void* lpMem, size_t dwBytes);
    static PAPAYA_MS_ABI void* hle_local_alloc(u32 uFlags, size_t uBytes);
    static PAPAYA_MS_ABI void* hle_local_free(void* hMem);

    // KERNEL32: Threading, Fiber & TLS
    static PAPAYA_MS_ABI u32   hle_tls_alloc();
    static PAPAYA_MS_ABI BOOL  hle_tls_free(u32 dwTlsIndex);
    static PAPAYA_MS_ABI void* hle_tls_get_value(u32 dwTlsIndex);
    static PAPAYA_MS_ABI BOOL  hle_tls_set_value(u32 dwTlsIndex, void* lpTlsValue);
    static PAPAYA_MS_ABI HANDLE hle_create_thread(void* lpSec, size_t dwStack, void* lpStart, void* lpParam, u32 dwFlags, u32* lpId);
    static PAPAYA_MS_ABI u32   hle_get_current_thread_id();
    static PAPAYA_MS_ABI u32   hle_get_current_process_id();
    static PAPAYA_MS_ABI HANDLE hle_get_current_thread();
    static PAPAYA_MS_ABI HANDLE hle_get_current_process();
    static PAPAYA_MS_ABI void  hle_exit_process(u32 uExitCode);
    static PAPAYA_MS_ABI void  hle_exit_thread(u32 dwExitCode);
    static PAPAYA_MS_ABI void  hle_sleep(u32 dwMilliseconds);
    static PAPAYA_MS_ABI u32   hle_sleep_ex(u32 dwMilliseconds, BOOL bAlertable);
    static PAPAYA_MS_ABI BOOL  hle_switch_to_thread();

    // KERNEL32: Synchronization
    static PAPAYA_MS_ABI void  hle_init_critical_section(Win32CriticalSection* lpSection);
    static PAPAYA_MS_ABI BOOL  hle_init_critical_section_and_spin_count(Win32CriticalSection* lpSection, u32 dwSpinCount);
    static PAPAYA_MS_ABI void  hle_enter_critical_section(Win32CriticalSection* lpSection);
    static PAPAYA_MS_ABI BOOL  hle_try_enter_critical_section(Win32CriticalSection* lpSection);
    static PAPAYA_MS_ABI void  hle_leave_critical_section(Win32CriticalSection* lpSection);
    static PAPAYA_MS_ABI void  hle_delete_critical_section(Win32CriticalSection* lpSection);
    static PAPAYA_MS_ABI HANDLE hle_create_event_a(void* lpSec, BOOL bManualReset, BOOL bInitialState, const char* lpName);
    static PAPAYA_MS_ABI HANDLE hle_create_event_w(void* lpSec, BOOL bManualReset, BOOL bInitialState, const wchar_t* lpName);
    static PAPAYA_MS_ABI BOOL  hle_set_event(HANDLE hEvent);
    static PAPAYA_MS_ABI BOOL  hle_reset_event(HANDLE hEvent);
    static PAPAYA_MS_ABI HANDLE hle_create_mutex_a(void* lpSec, BOOL bInitialOwner, const char* lpName);
    static PAPAYA_MS_ABI BOOL  hle_release_mutex(HANDLE hMutex);
    static PAPAYA_MS_ABI u32   hle_wait_for_single_object(HANDLE hHandle, u32 dwMilliseconds);
    static PAPAYA_MS_ABI u32   hle_wait_for_multiple_objects(u32 nCount, const HANDLE* lpHandles, BOOL bWaitAll, u32 dwMilliseconds);

    // KERNEL32: File System & Paths
    static PAPAYA_MS_ABI HANDLE hle_create_file_a(const char* lpFileName, u32 dwAccess, u32 dwShare, void* lpSec, u32 dwDisp, u32 dwFlags, HANDLE hTemplate);
    static PAPAYA_MS_ABI HANDLE hle_create_file_w(const wchar_t* lpFileName, u32 dwAccess, u32 dwShare, void* lpSec, u32 dwDisp, u32 dwFlags, HANDLE hTemplate);
    static PAPAYA_MS_ABI BOOL   hle_read_file(HANDLE hFile, void* lpBuffer, u32 nNumberOfBytesToRead, u32* lpNumberOfBytesRead, void* lpOverlapped);
    static PAPAYA_MS_ABI BOOL   hle_write_file(HANDLE hFile, const void* lpBuffer, u32 nNumberOfBytesToWrite, u32* lpNumberOfBytesWritten, void* lpOverlapped);
    static PAPAYA_MS_ABI BOOL   hle_close_handle(HANDLE hObject);
    static PAPAYA_MS_ABI u32    hle_get_file_size(HANDLE hFile, u32* lpFileSizeHigh);
    static PAPAYA_MS_ABI u32    hle_set_file_pointer(HANDLE hFile, s32 lDistanceToMove, s32* lpDistanceToMoveHigh, u32 dwMoveMethod);
    static PAPAYA_MS_ABI u32    hle_get_file_attributes_a(const char* lpFileName);
    static PAPAYA_MS_ABI u32    hle_get_file_attributes_w(const wchar_t* lpFileName);
    static PAPAYA_MS_ABI BOOL   hle_get_full_path_name_a(const char* lpFileName, u32 nBufferLength, char* lpBuffer, char** lpFilePart);
    static PAPAYA_MS_ABI u32    hle_get_current_directory_a(u32 nBufferLength, char* lpBuffer);
    static PAPAYA_MS_ABI BOOL   hle_set_current_directory_a(const char* lpPathName);
    static PAPAYA_MS_ABI HANDLE hle_find_first_file_a(const char* lpFileName, Win32FileFindDataA* lpFindFileData);
    static PAPAYA_MS_ABI BOOL   hle_find_next_file_a(HANDLE hFindFile, Win32FileFindDataA* lpFindFileData);
    static PAPAYA_MS_ABI BOOL   hle_find_close(HANDLE hFindFile);

    // KERNEL32: Module & Library
    static PAPAYA_MS_ABI void* hle_get_proc_address(void* hModule, const char* lpProcName);
    static PAPAYA_MS_ABI void* hle_get_module_handle_a(const char* lpModuleName);
    static PAPAYA_MS_ABI void* hle_get_module_handle_w(const wchar_t* lpModuleName);
    static PAPAYA_MS_ABI void* hle_load_library_a(const char* lpLibFileName);
    static PAPAYA_MS_ABI void* hle_load_library_w(const wchar_t* lpLibFileName);
    static PAPAYA_MS_ABI BOOL  hle_free_library(void* hLibModule);
    static PAPAYA_MS_ABI u32   hle_get_module_file_name_a(void* hModule, char* lpFilename, u32 nSize);

    // KERNEL32: Environment, System & Clock
    static PAPAYA_MS_ABI void  hle_get_system_info(Win32SystemInfo* lpSystemInfo);
    static PAPAYA_MS_ABI void  hle_get_native_system_info(Win32SystemInfo* lpSystemInfo);
    static PAPAYA_MS_ABI BOOL  hle_is_processor_feature_present(u32 ProcessorFeature);
    static PAPAYA_MS_ABI const char* hle_get_command_line_a();
    static PAPAYA_MS_ABI const wchar_t* hle_get_command_line_w();
    static PAPAYA_MS_ABI u32   hle_get_environment_variable_a(const char* lpName, char* lpBuffer, u32 nSize);
    static PAPAYA_MS_ABI BOOL  hle_query_performance_counter(s64* lpPerformanceCount);
    static PAPAYA_MS_ABI BOOL  hle_query_performance_frequency(s64* lpFrequency);
    static PAPAYA_MS_ABI u32   hle_get_tick_count();
    static PAPAYA_MS_ABI u64   hle_get_tick_count_64();
    static PAPAYA_MS_ABI u32   hle_get_last_error();
    static PAPAYA_MS_ABI void  hle_set_last_error(u32 dwErrCode);

    // USER32 Emulation
    static PAPAYA_MS_ABI s32   hle_get_system_metrics(s32 nIndex);
    static PAPAYA_MS_ABI BOOL  hle_set_process_dpi_aware();
    static PAPAYA_MS_ABI BOOL  hle_get_client_rect(HWND hWnd, void* lpRect);
    static PAPAYA_MS_ABI BOOL  hle_get_window_rect(HWND hWnd, void* lpRect);
    static PAPAYA_MS_ABI BOOL  hle_peek_message_a(void* lpMsg, HWND hWnd, u32 wMsgFilterMin, u32 wMsgFilterMax, u32 wRemoveMsg);
    static PAPAYA_MS_ABI BOOL  hle_dispatch_message_a(const void* lpMsg);
    static PAPAYA_MS_ABI BOOL  hle_translate_message(const void* lpMsg);

    // XINPUT Emulation
    static PAPAYA_MS_ABI u32   hle_xinput_get_state(u32 dwUserIndex, void* pState);
    static PAPAYA_MS_ABI u32   hle_xinput_set_state(u32 dwUserIndex, void* pVibration);
    static PAPAYA_MS_ABI u32   hle_xinput_get_capabilities(u32 dwUserIndex, u32 dwFlags, void* pCapabilities);

    // Steamworks Clean-Room Emulation Direct Exports
    static PAPAYA_MS_ABI BOOL  hle_steam_api_init();
    static PAPAYA_MS_ABI void  hle_steam_api_shutdown();
    static PAPAYA_MS_ABI void  hle_steam_api_run_callbacks();
    static PAPAYA_MS_ABI BOOL  hle_steam_api_restart_app_if_necessary(u32 unOwnAppID);
    static PAPAYA_MS_ABI void* hle_steam_internal_create_interface(const char* ver);

private:
    std::shared_ptr<steam::SteamApiStub> steam_stub_;
    std::shared_ptr<input::VirtualXInputManager> input_mgr_;
    std::unordered_map<std::string, std::unordered_map<std::string, void*>> export_table_;
};

} // namespace papaya::win32
