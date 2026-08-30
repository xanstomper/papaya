#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/steam/steam_api_stub.hpp"
#include "papaya/input/virtual_xinput.hpp"
#include "papaya/win32/win32_window.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstdarg>

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

struct Win32FileFindDataW {
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
    uint16_t cFileName[260]{0};
    uint16_t cAlternateFileName[14]{0};
};

struct Win32FileAttributeData {
    u32 dwFileAttributes{0};
    u32 ftCreationTimeLow{0};
    u32 ftCreationTimeHigh{0};
    u32 ftLastAccessTimeLow{0};
    u32 ftLastAccessTimeHigh{0};
    u32 ftLastWriteTimeLow{0};
    u32 ftLastWriteTimeHigh{0};
    u32 nFileSizeHigh{0};
    u32 nFileSizeLow{0};
};

class Win32ApiHle {
public:
    explicit Win32ApiHle(
        std::shared_ptr<steam::SteamApiStub> steam_stub = nullptr,
        std::shared_ptr<input::VirtualXInputManager> input_mgr = nullptr
    );
    ~Win32ApiHle() = default;

    Result<> initialize();
    static void set_game_path(const std::string& path);

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
    static PAPAYA_MS_ABI u32   hle_fls_alloc(void* lpCallback);
    static PAPAYA_MS_ABI BOOL  hle_fls_free(u32 dwFlsIndex);
    static PAPAYA_MS_ABI void* hle_fls_get_value(u32 dwFlsIndex);
    static PAPAYA_MS_ABI BOOL  hle_fls_set_value(u32 dwFlsIndex, void* lpFlsData);
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
    static PAPAYA_MS_ABI BOOL  hle_init_critical_section_ex(Win32CriticalSection* lpSection, u32 dwSpinCount, u32 flags);
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
    static PAPAYA_MS_ABI BOOL   hle_set_file_pointer_ex(HANDLE hFile, int64_t liDistanceToMove, int64_t* lpNewFilePointer, u32 dwMoveMethod);
    static PAPAYA_MS_ABI u32    hle_get_file_attributes_a(const char* lpFileName);
    static PAPAYA_MS_ABI u32    hle_get_file_attributes_w(const wchar_t* lpFileName);
    static PAPAYA_MS_ABI BOOL   hle_get_file_attributes_ex_a(const char* lpFileName, int fInfoLevelId, void* lpFileInformation);
    static PAPAYA_MS_ABI BOOL   hle_get_file_attributes_ex_w(const wchar_t* lpFileName, int fInfoLevelId, void* lpFileInformation);
    static PAPAYA_MS_ABI u32    hle_get_full_path_name_a(const char* lpFileName, u32 nBufferLength, char* lpBuffer, char** lpFilePart);
    static PAPAYA_MS_ABI u32    hle_get_full_path_name_w(const wchar_t* lpFileName, u32 nBufferLength, wchar_t* lpBuffer, wchar_t** lpFilePart);
    static PAPAYA_MS_ABI u32    hle_get_current_directory_a(u32 nBufferLength, char* lpBuffer);
    static PAPAYA_MS_ABI u32    hle_get_current_directory_w(u32 nBufferLength, wchar_t* lpBuffer);
    static PAPAYA_MS_ABI BOOL   hle_set_current_directory_a(const char* lpPathName);
    static PAPAYA_MS_ABI BOOL   hle_set_current_directory_w(const wchar_t* lpPathName);
    static PAPAYA_MS_ABI BOOL   hle_get_file_size_ex(HANDLE hFile, int64_t* lpFileSize);
    static PAPAYA_MS_ABI BOOL   hle_create_directory_w(const wchar_t* lpPathName, void* lpSec);
    static PAPAYA_MS_ABI BOOL   hle_delete_file_w(const wchar_t* lpFileName);
    static PAPAYA_MS_ABI BOOL   hle_get_file_information_by_handle(HANDLE hFile, void* lpFileInformation);
    static PAPAYA_MS_ABI u32    hle_get_file_type(HANDLE hFile);
    static PAPAYA_MS_ABI u16    hle_get_user_default_ui_language();
    static PAPAYA_MS_ABI u32    hle_get_user_default_lcid();
    static PAPAYA_MS_ABI int    hle_get_locale_info_ex(const wchar_t* lpLocaleName, u32 LCType, wchar_t* lpLCData, int cchData);
    static PAPAYA_MS_ABI int    hle_lc_map_string_w(u32 Locale, u32 dwMapFlags, const wchar_t* lpSrcStr, int cchSrc, wchar_t* lpDestStr, int cchDest);
    static PAPAYA_MS_ABI int    hle_compare_string_w(u32 Locale, u32 dwCmpFlags, const wchar_t* lpString1, int cchCount1, const wchar_t* lpString2, int cchCount2);
    static PAPAYA_MS_ABI int    hle_compare_string_ordinal(const wchar_t* lpString1, int cchCount1, const wchar_t* lpString2, int cchCount2, BOOL bIgnoreCase);
    static PAPAYA_MS_ABI int    hle_compare_file_time(const void* lpFileTime1, const void* lpFileTime2);
    static PAPAYA_MS_ABI BOOL   hle_system_time_to_tz_specific_local_time(const void* lpTimeZoneInformation, const void* lpUniversalTime, void* lpLocalTime);
    static PAPAYA_MS_ABI u32    hle_get_time_zone_information(void* lpTimeZoneInformation);
    static PAPAYA_MS_ABI BOOL   hle_get_volume_information_w(const wchar_t* lpRootPathName, wchar_t* lpVolumeNameBuffer, u32 nVolumeNameSize, u32* lpVolumeSerialNumber, u32* lpMaximumComponentLength, u32* lpFileSystemFlags, wchar_t* lpFileSystemNameBuffer, u32 nFileSystemNameSize);
    static PAPAYA_MS_ABI BOOL   hle_get_disk_free_space_ex_a(const char* lpDirectoryName, uint64_t* lpFreeBytesAvailableToCaller, uint64_t* lpTotalNumberOfBytes, uint64_t* lpTotalNumberOfFreeBytes);
    static PAPAYA_MS_ABI BOOL   hle_get_disk_free_space_ex_w(const wchar_t* lpDirectoryName, uint64_t* lpFreeBytesAvailableToCaller, uint64_t* lpTotalNumberOfBytes, uint64_t* lpTotalNumberOfFreeBytes);
    static PAPAYA_MS_ABI u32   hle_expand_environment_strings_a(const char* lpSrc, char* lpDst, u32 nSize);
    static PAPAYA_MS_ABI u32   hle_expand_environment_strings_w(const wchar_t* lpSrc, wchar_t* lpDst, u32 nSize);
    static PAPAYA_MS_ABI BOOL  hle_set_file_attributes_a(const char* lpFileName, u32 dwFileAttributes);
    static PAPAYA_MS_ABI BOOL  hle_set_file_attributes_w(const wchar_t* lpFileName, u32 dwFileAttributes);
    static PAPAYA_MS_ABI BOOL  hle_set_file_time(HANDLE hFile, const void* lpCreation, const void* lpLastAccess, const void* lpLastWrite);
    static PAPAYA_MS_ABI u32   hle_suspend_thread(HANDLE hThread);
    static PAPAYA_MS_ABI u32   hle_resume_thread(HANDLE hThread);
    static PAPAYA_MS_ABI HANDLE hle_create_toolhelp32_snapshot(u32 dwFlags, u32 th32ProcessID);
    static PAPAYA_MS_ABI BOOL  hle_thread32_first(HANDLE hSnapshot, void* lpte);
    static PAPAYA_MS_ABI BOOL  hle_thread32_next(HANDLE hSnapshot, void* lpte);
    static PAPAYA_MS_ABI s32   hle_ole_initialize(const void* pvReserved);
    static PAPAYA_MS_ABI void  hle_ole_uninitialize();
    static PAPAYA_MS_ABI s32   hle_register_drag_drop(void* hwnd, void* pDropTarget);
    static PAPAYA_MS_ABI s32   hle_revoke_drag_drop(void* hwnd);
    static PAPAYA_MS_ABI s32   hle_set_error_info(u32 dwReserved, void* perrinfo);
    static PAPAYA_MS_ABI s32   hle_get_error_info(u32 dwReserved, void** pperrinfo);
    static PAPAYA_MS_ABI s32   hle_create_error_info(u32 dwReserved, void** pperrinfo);
    static PAPAYA_MS_ABI s32   hle_co_create_free_threaded_marshaler(void* pOuter, void** ppMarshal);
    static PAPAYA_MS_ABI void  hle_release_stg_medium(void* pmedium);
    static PAPAYA_MS_ABI u32   hle_com_release(void* pUnk);
    static PAPAYA_MS_ABI u32    hle_get_logical_drives();
    static PAPAYA_MS_ABI u32    hle_get_temp_file_name_w(const wchar_t* lpPathName, const wchar_t* lpPrefixString, u32 uUnique, wchar_t* lpTempFileName);
    static PAPAYA_MS_ABI BOOL   hle_replace_file_w(const wchar_t* lpReplacedFileName, const wchar_t* lpReplacementFileName, const wchar_t* lpBackupFileName, u32 dwReplaceFlags, void* lpExclude, void* lpReserved);
    static PAPAYA_MS_ABI BOOL   hle_move_file_ex_w(const wchar_t* lpExistingFileName, const wchar_t* lpNewFileName, u32 dwFlags);
    static PAPAYA_MS_ABI BOOL   hle_remove_directory_w(const wchar_t* lpPathName);
    static PAPAYA_MS_ABI u32    hle_get_drive_type_w(const wchar_t* lpRootPathName);
    static PAPAYA_MS_ABI HANDLE hle_get_std_handle(u32 nStdHandle);
    static PAPAYA_MS_ABI BOOL   hle_set_std_handle(u32 nStdHandle, HANDLE hHandle);
    static PAPAYA_MS_ABI s32    hle_unhandled_exception_filter(void* ExceptionInfo);
    static PAPAYA_MS_ABI BOOL   hle_set_end_of_file(HANDLE hFile);
    static PAPAYA_MS_ABI BOOL   hle_flush_file_buffers(HANDLE hFile);
    static PAPAYA_MS_ABI BOOL   hle_peek_named_pipe(HANDLE hNamedPipe, void* lpBuffer, u32 nBufferSize, u32* lpBytesRead, u32* lpTotalBytesAvail, u32* lpBytesLeftThisMessage);
    static PAPAYA_MS_ABI BOOL   hle_create_pipe(HANDLE* hReadPipe, HANDLE* hWritePipe, void* lpPipeAttributes, u32 nSize);
    static PAPAYA_MS_ABI BOOL   hle_set_handle_information(HANDLE hObject, u32 dwMask, u32 dwFlags);
    static PAPAYA_MS_ABI BOOL   hle_get_exit_code_process(HANDLE hProcess, u32* lpExitCode);
    static PAPAYA_MS_ABI BOOL   hle_get_exit_code_thread(HANDLE hThread, u32* lpExitCode);
    static PAPAYA_MS_ABI HANDLE hle_open_process(u32 dwDesiredAccess, BOOL bInheritHandle, u32 dwProcessId);
    static PAPAYA_MS_ABI BOOL   hle_create_process_w(const wchar_t* lpApp, wchar_t* lpCmd, void* lpPA, void* lpTA, BOOL bInherit, u32 dwFlags, void* lpEnv, const wchar_t* lpCurrDir, void* lpSI, void* lpPI);
    static PAPAYA_MS_ABI wchar_t* hle_get_environment_strings_w();
    static PAPAYA_MS_ABI BOOL   hle_free_environment_strings_w(wchar_t* penv);
    static PAPAYA_MS_ABI BOOL   hle_set_environment_variable_w(const wchar_t* lpName, const wchar_t* lpValue);
    static PAPAYA_MS_ABI u32    hle_get_environment_variable_w(const wchar_t* lpName, wchar_t* lpBuffer, u32 nSize);
    static PAPAYA_MS_ABI u32    hle_get_oemcp();
    static PAPAYA_MS_ABI BOOL   hle_is_valid_code_page(u32 CodePage);
    static PAPAYA_MS_ABI BOOL   hle_is_valid_locale(u32 Locale, u32 dwFlags);
    static PAPAYA_MS_ABI BOOL   hle_enum_system_locales_w(void* lpLocaleEnumProc, u32 dwFlags);
    static PAPAYA_MS_ABI BOOL   hle_get_string_type_w(u32 dwInfoType, const wchar_t* lpSrcStr, int cchSrc, u16* lpCharType);
    static PAPAYA_MS_ABI BOOL   hle_get_cpinfo(u32 CodePage, void* lpCPInfo);
    static PAPAYA_MS_ABI u32    hle_set_thread_ideal_processor(HANDLE hThread, u32 dwIdealProcessor);
    static PAPAYA_MS_ABI uint64_t hle_set_thread_affinity_mask(HANDLE hThread, uint64_t dwThreadAffinityMask);
    static PAPAYA_MS_ABI BOOL   hle_set_thread_priority(HANDLE hThread, int nPriority);
    static PAPAYA_MS_ABI BOOL   hle_set_priority_class(HANDLE hProcess, u32 dwPriorityClass);
    static PAPAYA_MS_ABI HANDLE hle_power_create_request(void* Context);
    static PAPAYA_MS_ABI BOOL   hle_power_set_request(HANDLE PowerRequest, u32 RequestType);
    static PAPAYA_MS_ABI BOOL   hle_power_clear_request(HANDLE PowerRequest, u32 RequestType);
    static PAPAYA_MS_ABI void   hle_initialize_srw_lock(void* SRWLock);
    static PAPAYA_MS_ABI void   hle_acquire_srw_lock_exclusive(void* SRWLock);
    static PAPAYA_MS_ABI void   hle_release_srw_lock_exclusive(void* SRWLock);
    static PAPAYA_MS_ABI BOOL   hle_try_acquire_srw_lock_exclusive(void* SRWLock);
    static PAPAYA_MS_ABI void   hle_acquire_srw_lock_shared(void* SRWLock);
    static PAPAYA_MS_ABI void   hle_release_srw_lock_shared(void* SRWLock);
    static PAPAYA_MS_ABI void   hle_initialize_condition_variable(void* ConditionVariable);
    static PAPAYA_MS_ABI void   hle_wake_condition_variable(void* ConditionVariable);
    static PAPAYA_MS_ABI void   hle_wake_all_condition_variable(void* ConditionVariable);
    static PAPAYA_MS_ABI BOOL   hle_sleep_condition_variable_cs(void* ConditionVariable, Win32CriticalSection* CriticalSection, u32 dwMilliseconds);
    static PAPAYA_MS_ABI BOOL   hle_sleep_condition_variable_srw(void* ConditionVariable, void* SRWLock, u32 dwMilliseconds, u32 Flags);
    static PAPAYA_MS_ABI BOOL   hle_init_once_begin_initialize(void* InitOnce, u32 dwFlags, BOOL* fPending, void** lpContext);
    static PAPAYA_MS_ABI BOOL   hle_init_once_complete(void* InitOnce, u32 dwFlags, void* lpContext);
    static PAPAYA_MS_ABI void   hle_initialize_slist_head(void* ListHead);
    static PAPAYA_MS_ABI void*  hle_interlocked_push_entry_slist(void* ListHead, void* ListEntry);
    static PAPAYA_MS_ABI void*  hle_global_alloc(u32 uFlags, size_t uBytes);
    static PAPAYA_MS_ABI void*  hle_global_free(void* hMem);
    static PAPAYA_MS_ABI void*  hle_global_lock(void* hMem);
    static PAPAYA_MS_ABI BOOL   hle_global_unlock(void* hMem);
    static PAPAYA_MS_ABI HANDLE hle_find_first_file_a(const char* lpFileName, Win32FileFindDataA* lpFindFileData);
    static PAPAYA_MS_ABI HANDLE hle_find_first_file_w(const wchar_t* lpFileName, Win32FileFindDataW* lpFindFileData);
    static PAPAYA_MS_ABI HANDLE hle_find_first_file_ex_w(const wchar_t* lpFileName, int fInfoLevelId, Win32FileFindDataW* lpFindFileData, int fSearchOp, void* lpSearchFilter, u32 dwAdditionalFlags);
    static PAPAYA_MS_ABI BOOL   hle_find_next_file_a(HANDLE hFindFile, Win32FileFindDataA* lpFindFileData);
    static PAPAYA_MS_ABI BOOL   hle_find_next_file_w(HANDLE hFindFile, Win32FileFindDataW* lpFindFileData);
    static PAPAYA_MS_ABI BOOL   hle_find_close(HANDLE hFindFile);

    // KERNEL32: Module & Library
    static PAPAYA_MS_ABI void* hle_get_proc_address(void* hModule, const char* lpProcName);
    static PAPAYA_MS_ABI void* hle_get_module_handle_a(const char* lpModuleName);
    static PAPAYA_MS_ABI void* hle_get_module_handle_w(const wchar_t* lpModuleName);
    static PAPAYA_MS_ABI void* hle_load_library_a(const char* lpLibFileName);
    static PAPAYA_MS_ABI void* hle_load_library_w(const wchar_t* lpLibFileName);
    static PAPAYA_MS_ABI BOOL  hle_free_library(void* hLibModule);
    static PAPAYA_MS_ABI u32   hle_get_module_file_name_a(void* hModule, char* lpFilename, u32 nSize);
    static PAPAYA_MS_ABI void* hle_encode_pointer(void* ptr);
    static PAPAYA_MS_ABI void* hle_decode_pointer(void* ptr);
    static PAPAYA_MS_ABI void* hle_encode_system_pointer(void* ptr);
    static PAPAYA_MS_ABI void* hle_decode_system_pointer(void* ptr);
    static PAPAYA_MS_ABI u32   hle_get_current_processor_number();
    static PAPAYA_MS_ABI void* hle_interlocked_flush_slist(void* head);

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
    static PAPAYA_MS_ABI char* hle_lstrcpy_a(char* dst, const char* src);
    static PAPAYA_MS_ABI int   hle_lstrcmp_a(const char* a, const char* b);
    static PAPAYA_MS_ABI u32   hle_get_thread_priority(void* hThread);
    static PAPAYA_MS_ABI u32   hle_get_private_profile_string_a(const char* app, const char* key, const char* def, char* out, u32 size, const char* file);
    static PAPAYA_MS_ABI BOOL  hle_write_private_profile_string_a(const char* app, const char* key, const char* value, const char* file);
    static PAPAYA_MS_ABI u32   hle_get_last_error();
    static PAPAYA_MS_ABI void  hle_set_last_error(u32 dwErrCode);

    // USER32 Emulation
    static PAPAYA_MS_ABI s32   hle_get_system_metrics(s32 nIndex);
    static PAPAYA_MS_ABI BOOL  hle_set_process_dpi_aware();
    static PAPAYA_MS_ABI BOOL  hle_get_client_rect(HWND hWnd, void* lpRect);
    static PAPAYA_MS_ABI BOOL  hle_get_window_rect(HWND hWnd, void* lpRect);
    static PAPAYA_MS_ABI BOOL  hle_peek_message_a(void* lpMsg, HWND hWnd, u32 wMsgFilterMin, u32 wMsgFilterMax, u32 wRemoveMsg);
    static PAPAYA_MS_ABI BOOL  hle_get_message_a(void* lpMsg, HWND hWnd, u32 wMsgFilterMin, u32 wMsgFilterMax);
    static PAPAYA_MS_ABI BOOL  hle_dispatch_message_a(const void* lpMsg);
    static PAPAYA_MS_ABI BOOL  hle_translate_message(const void* lpMsg);
    // Window creation / management (X11-backed)
    static PAPAYA_MS_ABI void* hle_register_class_a(const void* lpWndClass);
    static PAPAYA_MS_ABI void* hle_register_class_w(const void* lpWndClass);
    static PAPAYA_MS_ABI void* hle_create_window_ex_a(u32 dwExStyle, const char* lpClassName,
                             const char* lpWindowName, u32 dwStyle, int x, int y, int w, int h,
                             void* hWndParent, void* hMenu, void* hInstance, void* lpParam);
    static PAPAYA_MS_ABI void* hle_create_window_ex_w(u32 dwExStyle, const wchar_t* lpClassName,
                             const wchar_t* lpWindowName, u32 dwStyle, int x, int y, int w, int h,
                             void* hWndParent, void* hMenu, void* hInstance, void* lpParam);
    static PAPAYA_MS_ABI BOOL  hle_destroy_window(HWND hWnd);
    static PAPAYA_MS_ABI BOOL  hle_show_window(HWND hWnd, int nCmdShow);
    static PAPAYA_MS_ABI BOOL  hle_update_window(HWND hWnd);
    static PAPAYA_MS_ABI s64   hle_def_window_proc_a(HWND hWnd, u32 msg, u64 wParam, s64 lParam);
    static PAPAYA_MS_ABI s64   hle_def_window_proc_w(HWND hWnd, u32 msg, u64 wParam, s64 lParam);
    static PAPAYA_MS_ABI void  hle_post_quit_message(int nExitCode);
    static PAPAYA_MS_ABI BOOL  hle_post_message_a(HWND hWnd, u32 msg, u64 wParam, s64 lParam);
    static PAPAYA_MS_ABI BOOL  hle_post_message_w(HWND hWnd, u32 msg, u64 wParam, s64 lParam);
    static PAPAYA_MS_ABI s64   hle_send_message_a(HWND hWnd, u32 msg, u64 wParam, s64 lParam);
    static PAPAYA_MS_ABI u32   hle_register_window_message_a(const char* lpString);
    static PAPAYA_MS_ABI void* hle_load_icon_a(void* hInstance, const char* lpIconName);
    static PAPAYA_MS_ABI void* hle_load_cursor_a(void* hInstance, const char* lpCursorName);
    static PAPAYA_MS_ABI s64   hle_get_class_long_a(HWND hWnd, int nIndex);
    static PAPAYA_MS_ABI s64   hle_set_class_long_a(HWND hWnd, int nIndex, s64 dwNewLong);
    static PAPAYA_MS_ABI s64   hle_get_window_long_a(HWND hWnd, int nIndex);
    static PAPAYA_MS_ABI s64   hle_set_window_long_a(HWND hWnd, int nIndex, s64 dwNewLong);
    static PAPAYA_MS_ABI BOOL  hle_system_parameters_info_a(u32 uiAction, u32 uiParam, void* pvParam, u32 fWinIni);
        static PAPAYA_MS_ABI void* hle_get_desktop_window();
        static PAPAYA_MS_ABI BOOL  hle_client_to_screen(HWND hWnd, void* lpPoint);
        static PAPAYA_MS_ABI BOOL  hle_screen_to_client(HWND hWnd, void* lpPoint);
        static PAPAYA_MS_ABI void* hle_create_font_indirect_a(const void* lpLogFont);
        static PAPAYA_MS_ABI u32   hle_map_virtual_key_a(u32 uCode, u32 uMapType);
    static PAPAYA_MS_ABI s64   hle_send_message_w(HWND hWnd, u32 msg, u64 wParam, s64 lParam);
    static PAPAYA_MS_ABI void* hle_get_dc(HWND hWnd);
    static PAPAYA_MS_ABI int   hle_release_dc(HWND hWnd, void* hDC);
    static PAPAYA_MS_ABI void* hle_begin_paint(HWND hWnd, void* ps);
    static PAPAYA_MS_ABI BOOL  hle_end_paint(HWND hWnd, const void* ps);
    static PAPAYA_MS_ABI BOOL  hle_invalidate_rect(HWND hWnd, const void* lpRect, BOOL bErase);

    // XINPUT Emulation
    static PAPAYA_MS_ABI u32   hle_xinput_get_state(u32 dwUserIndex, void* pState);
    static PAPAYA_MS_ABI u32   hle_xinput_set_state(u32 dwUserIndex, void* pVibration);
    static PAPAYA_MS_ABI u32   hle_xinput_get_capabilities(u32 dwUserIndex, u32 dwFlags, void* pCapabilities);
    static PAPAYA_MS_ABI void  hle_xinput_enable(BOOL bEnable);
    static PAPAYA_MS_ABI u32   hle_xinput_get_battery_info(u32 dwUserIndex, u8 devType, void* pBattery);
    static PAPAYA_MS_ABI u32   hle_xinput_get_keystroke(u32 dwUserIndex, u32 dwReserved, void* pKeystroke);
    static PAPAYA_MS_ABI u32   hle_xinput_get_dsound_audio_device_guids(u32 dwUserIndex, void* pDSoundRenderGuid, void* pDSoundCaptureGuid);
    static PAPAYA_MS_ABI u32   hle_xinput_get_audio_device_ids(u32 dwUserIndex, void* pRenderId, u32* pRenderCount, void* pCaptureId, u32* pCaptureCount);

    // Steamworks Clean-Room Emulation Direct Exports
    static PAPAYA_MS_ABI BOOL  hle_steam_api_init();
    static PAPAYA_MS_ABI void  hle_steam_api_shutdown();
    static PAPAYA_MS_ABI void  hle_steam_api_run_callbacks();
    static PAPAYA_MS_ABI BOOL  hle_steam_api_restart_app_if_necessary(u32 unOwnAppID);
    static PAPAYA_MS_ABI void* hle_steam_internal_create_interface(const char* ver);
    static PAPAYA_MS_ABI void* hle_steam_internal_context_init(void* pCtxPointer);
    static PAPAYA_MS_ABI void* hle_steam_internal_find_or_create_user_interface(const char* ver);
    static PAPAYA_MS_ABI void* hle_steam_internal_find_or_create_server_interface(const char* ver);
    static PAPAYA_MS_ABI void  hle_steam_register_callback(int cb, int nCallback);
    static PAPAYA_MS_ABI void  hle_steam_unregister_callback(int cb);
    static PAPAYA_MS_ABI void  hle_steam_register_call_result(int cb, int hResult);
    static PAPAYA_MS_ABI void  hle_steam_unregister_call_result(int cb);
    static PAPAYA_MS_ABI BOOL  hle_steam_is_running();
    static PAPAYA_MS_ABI u32   hle_steam_get_h_steam_user();

    // MSVCRT Emulation (the C runtime every mingw/MSVC binary needs).
    // Memory & string (void-returning int for ABI simplicity; see impls).
    static PAPAYA_MS_ABI void* hle_msvcrt_malloc(size_t);
    static PAPAYA_MS_ABI void  hle_msvcrt_free(void*);
    static PAPAYA_MS_ABI void* hle_msvcrt_calloc(size_t, size_t);
    static PAPAYA_MS_ABI void* hle_msvcrt_realloc(void*, size_t);
    static PAPAYA_MS_ABI void* hle_msvcrt_memcpy(void*, const void*, size_t);
    static PAPAYA_MS_ABI void* hle_msvcrt_memmove(void*, const void*, size_t);
    static PAPAYA_MS_ABI void* hle_msvcrt_memset(void*, int, size_t);
    static PAPAYA_MS_ABI size_t hle_msvcrt_strlen(const char*);
    static PAPAYA_MS_ABI int   hle_msvcrt_strcmp(const char*, const char*);
    static PAPAYA_MS_ABI int   hle_msvcrt_strncmp(const char*, const char*, size_t);
    static PAPAYA_MS_ABI char* hle_msvcrt_strcpy(char*, const char*);
    static PAPAYA_MS_ABI char* hle_msvcrt_strncpy(char*, const char*, size_t);
    static PAPAYA_MS_ABI char* hle_msvcrt_strcat(char*, const char*);
    static PAPAYA_MS_ABI int   hle_msvcrt_atoi(const char*);
    static PAPAYA_MS_ABI double hle_msvcrt_atof(const char*);
    static PAPAYA_MS_ABI void* hle_msvcrt_mbstowcs(void*, const char*, size_t);

    // Process lifecycle / CRT init
    static PAPAYA_MS_ABI void  hle_msvcrt_exit(int);
    static PAPAYA_MS_ABI void  hle_msvcrt__exit(int);
    static PAPAYA_MS_ABI void  hle_msvcrt_abort();
    static PAPAYA_MS_ABI void  hle_msvcrt__cexit();
    static PAPAYA_MS_ABI int   hle_msvcrt__initterm(void*, void*);
    static PAPAYA_MS_ABI void  hle_msvcrt__set_app_type(int);
    static PAPAYA_MS_ABI void  hle_msvcrt__amsg_exit(int);
    static PAPAYA_MS_ABI void* hle_msvcrt__onexit(void* fn);
    static PAPAYA_MS_ABI int   hle_msvcrt__ismbblead(u32);
    static PAPAYA_MS_ABI void  hle_msvcrt__getmainargs(int*, char***, char***, int, int*);
    static PAPAYA_MS_ABI void  hle_msvcrt__setusermatherr(void*);

    // stdio
    static PAPAYA_MS_ABI int   hle_msvcrt_printf(const char*, ...);
    static PAPAYA_MS_ABI int   hle_msvcrt_fprintf(void*, const char*, ...);
    static PAPAYA_MS_ABI int   hle_msvcrt_vfprintf(void*, const char*, va_list);
    static PAPAYA_MS_ABI int   hle_msvcrt_sprintf(char*, const char*, ...);
    static PAPAYA_MS_ABI int   hle_user32_wsprintf_a(char* buf, const char* fmt, ...);
    static PAPAYA_MS_ABI size_t hle_msvcrt_fwrite(const void*, size_t, size_t, void*);
    static PAPAYA_MS_ABI int   hle_msvcrt_puts(const char*);
    static PAPAYA_MS_ABI int   hle_msvcrt_fputs(const char*, void*);
    static PAPAYA_MS_ABI int   hle_msvcrt_fputc(int, void*);
    static PAPAYA_MS_ABI void* hle_msvcrt___iob_func();

    // stdio + locale (real, host-backed; remove unresolved-import boot noise)
    static PAPAYA_MS_ABI int   hle_msvcrt_fflush(void* stream);
    static PAPAYA_MS_ABI char* hle_msvcrt_strerror(int errnum);
    static PAPAYA_MS_ABI void* hle_msvcrt_localeconv();
    static PAPAYA_MS_ABI void  hle_msvcrt_lock(int locknum);
    static PAPAYA_MS_ABI void  hle_msvcrt_unlock(int locknum);
    static PAPAYA_MS_ABI int   hle_msvcrt_lc_codepage_func();
    static PAPAYA_MS_ABI int   hle_msvcrt_mb_cur_max_func();

    // KERNEL32
    static PAPAYA_MS_ABI BOOL  hle_isdbcs_lead_byte_ex(u32 codepage, u8 byte);

    // Misc / signal
    static PAPAYA_MS_ABI void* hle_msvcrt_signal(int, void*);
    static PAPAYA_MS_ABI void  hle_msvcrt__commode(int);
    static PAPAYA_MS_ABI void  hle_msvcrt__fmode(int);
    static PAPAYA_MS_ABI void* hle_msvcrt___C_specific_handler(void*, void*, void*, void*);
    static PAPAYA_MS_ABI int   hle_msvcrt__crt_debugger_hook(int);

    // KERNEL32 additions
    static PAPAYA_MS_ABI void  hle_get_startup_info_a(void*);
    static PAPAYA_MS_ABI void  hle_get_startup_info_w(void*);
    static PAPAYA_MS_ABI void* hle_set_unhandled_exception_filter(void*);
    static PAPAYA_MS_ABI size_t hle_virtual_query(void*, int, void*, size_t);

    // Semaphore
    static PAPAYA_MS_ABI HANDLE hle_create_semaphore_a(void* lpSec, s32 lInitialCount, s32 lMaxCount, const char* lpName);
    static PAPAYA_MS_ABI HANDLE hle_create_semaphore_w(void* lpSec, s32 lInitialCount, s32 lMaxCount, const wchar_t* lpName);
    static PAPAYA_MS_ABI BOOL   hle_release_semaphore(HANDLE hSemaphore, s32 lReleaseCount, s32* lpPreviousCount);
    static PAPAYA_MS_ABI HANDLE hle_open_semaphore_a(u32 dwAccess, BOOL bInherit, const char* lpName);

    // Named event/mutex open
    static PAPAYA_MS_ABI HANDLE hle_open_event_a(u32 dwAccess, BOOL bInherit, const char* lpName);
    static PAPAYA_MS_ABI HANDLE hle_open_event_w(u32 dwAccess, BOOL bInherit, const wchar_t* lpName);
    static PAPAYA_MS_ABI HANDLE hle_open_mutex_a(u32 dwAccess, BOOL bInherit, const char* lpName);

    // Interlocked atomics
    static PAPAYA_MS_ABI s32  hle_interlocked_increment(volatile s32* lpAddend);
    static PAPAYA_MS_ABI s32  hle_interlocked_decrement(volatile s32* lpAddend);
    static PAPAYA_MS_ABI s32  hle_interlocked_exchange(volatile s32* Target, s32 Value);
    static PAPAYA_MS_ABI s32  hle_interlocked_compare_exchange(volatile s32* Dest, s32 Exchange, s32 Comparand);
    static PAPAYA_MS_ABI s64  hle_interlocked_exchange_add(volatile s64* Addend, s64 Value);

    // Extended wait
    static PAPAYA_MS_ABI u32  hle_wait_for_single_object_ex(HANDLE hHandle, u32 dwMilliseconds, BOOL bAlertable);

    // Handle duplication
    static PAPAYA_MS_ABI BOOL hle_duplicate_handle(HANDLE hSrcProc, HANDLE hSrcHandle, HANDLE hDstProc, HANDLE* lpTargetHandle, u32 dwAccess, BOOL bInherit, u32 dwOptions);

    // Instruction cache flush (no-op on x86; required on ARM)
    static PAPAYA_MS_ABI BOOL hle_flush_instruction_cache(HANDLE hProcess, const void* lpBaseAddress, size_t dwSize);

    // Console & debug
    static PAPAYA_MS_ABI BOOL  hle_set_console_ctrl_handler(void* HandlerRoutine, BOOL Add);
    static PAPAYA_MS_ABI void  hle_output_debug_string_a(const char* lpOutputString);
    static PAPAYA_MS_ABI void  hle_output_debug_string_w(const wchar_t* lpOutputString);
    static PAPAYA_MS_ABI BOOL  hle_is_debugger_present();

    // Wide char / multibyte conversion
    static PAPAYA_MS_ABI int   hle_multi_byte_to_wide_char(u32 CodePage, u32 dwFlags, const char* lpMBStr, int cbMB, wchar_t* lpWStr, int cchWC);
    static PAPAYA_MS_ABI int   hle_wide_char_to_multi_byte(u32 CodePage, u32 dwFlags, const wchar_t* lpWStr, int cchWC, char* lpMBStr, int cbMB, const char* lpDef, BOOL* lpUsed);

    // Locale & format
    static PAPAYA_MS_ABI int   hle_get_locale_info_a(u32 Locale, u32 LCType, char* lpLCData, int cchData);
    static PAPAYA_MS_ABI int   hle_get_locale_info_w(u32 Locale, u32 LCType, wchar_t* lpLCData, int cchData);
    static PAPAYA_MS_ABI u32   hle_get_acp();
    static PAPAYA_MS_ABI u32   hle_get_system_default_locale_name(wchar_t* lpLocaleName, int cchLocaleName);
    static PAPAYA_MS_ABI u32   hle_format_message_a(u32 dwFlags, const void* lpSource, u32 dwMsgId, u32 dwLangId, char* lpBuf, u32 nSize, void* args);
    static PAPAYA_MS_ABI u32   hle_format_message_w(u32 dwFlags, const void* lpSource, u32 dwMsgId, u32 dwLangId, wchar_t* lpBuf, u32 nSize, void* args);

    // Date/time string formatting
    static PAPAYA_MS_ABI int   hle_get_date_format_a(u32 Locale, u32 dwFlags, const void* lpDate, const char* lpFormat, char* lpDateStr, int cchDate);
    static PAPAYA_MS_ABI int   hle_get_time_format_a(u32 Locale, u32 dwFlags, const void* lpTime, const char* lpFormat, char* lpTimeStr, int cchTime);

    // System time
    static PAPAYA_MS_ABI void  hle_get_system_time(void* lpSystemTime);
    static PAPAYA_MS_ABI void  hle_get_local_time(void* lpSystemTime);
    static PAPAYA_MS_ABI BOOL  hle_system_time_to_file_time(const void* lpSystemTime, void* lpFileTime);
    static PAPAYA_MS_ABI BOOL  hle_file_time_to_system_time(const void* lpFileTime, void* lpSystemTime);

    // Misc kernel
    static PAPAYA_MS_ABI BOOL  hle_create_directory_a(const char* lpPathName, void* lpSec);
    static PAPAYA_MS_ABI BOOL  hle_remove_directory_a(const char* lpPathName);
    static PAPAYA_MS_ABI BOOL  hle_delete_file_a(const char* lpFileName);
    static PAPAYA_MS_ABI BOOL  hle_copy_file_a(const char* lpExisting, const char* lpNew, BOOL bFailIfExists);
    static PAPAYA_MS_ABI BOOL  hle_move_file_a(const char* lpExisting, const char* lpNew);
    static PAPAYA_MS_ABI u32   hle_get_temp_path_a(u32 nBufferLength, char* lpBuffer);
    static PAPAYA_MS_ABI BOOL  hle_copy_file_w(const wchar_t* lpExisting, const wchar_t* lpNew, BOOL bFailIfExists);
    static PAPAYA_MS_ABI BOOL  hle_set_environment_variable_a(const char* lpName, const char* lpValue);
    static PAPAYA_MS_ABI HANDLE hle_open_file(const char* lpFileName, u32* lpReOpenBuff, u32 uStyle, u32 uExclusive);
    static PAPAYA_MS_ABI u32   hle_get_temp_file_name_a(const char* lpPathName, const char* lpPrefixStr, u32 uUnique, char* lpTempFileName);
    static PAPAYA_MS_ABI u32   hle_get_windows_directory_a(char* lpBuffer, u32 uSize);
    static PAPAYA_MS_ABI u32   hle_get_system_directory_a(char* lpBuffer, u32 uSize);
    static PAPAYA_MS_ABI BOOL  hle_get_computer_name_a(char* lpBuffer, u32* lpnSize);
    static PAPAYA_MS_ABI BOOL  hle_get_user_name_a(char* lpBuffer, u32* lpnSize);
    // USER32 window-state helpers
    static PAPAYA_MS_ABI void* hle_set_cursor(void* hCursor);
    static PAPAYA_MS_ABI void* hle_get_foreground_window();
    static PAPAYA_MS_ABI BOOL  hle_set_foreground_window(void* hwnd);
    static PAPAYA_MS_ABI void* hle_get_active_window();
    static PAPAYA_MS_ABI void* hle_set_active_window(void* hwnd);
    static PAPAYA_MS_ABI void* hle_get_focus();
    static PAPAYA_MS_ABI void* hle_set_focus(void* hwnd);
    static PAPAYA_MS_ABI void* hle_get_capture();
    static PAPAYA_MS_ABI void* hle_set_capture(void* hwnd);
    static PAPAYA_MS_ABI BOOL  hle_release_capture();
    static PAPAYA_MS_ABI int   hle_message_box_a(void* hwnd, const char* text, const char* caption, u32 type);
    static PAPAYA_MS_ABI int   hle_message_box_w(void* hwnd, const wchar_t* text, const wchar_t* caption, u32 type);
    static PAPAYA_MS_ABI void* hle_get_environment_strings();
    static PAPAYA_MS_ABI BOOL  hle_free_environment_strings_a(void* lpszEnvironmentBlock);
    // ADVAPI32 Registry
    static PAPAYA_MS_ABI long  hle_reg_create_key_ex_a(u64 hKey, const char* lpSubKey, u32 reserved, void* lpClass, u32 dwOptions, u32 samDesired, void* lpSecurityAttr, u64* phkResult, u32* lpdwDisposition);
    static PAPAYA_MS_ABI long  hle_reg_create_key_ex_w(u64 hKey, const wchar_t* lpSubKey, u32 reserved, void* lpClass, u32 dwOptions, u32 samDesired, void* lpSecurityAttr, u64* phkResult, u32* lpdwDisposition);
    static PAPAYA_MS_ABI long  hle_reg_open_key_ex_a(u64 hKey, const char* lpSubKey, u32 ulOptions, u32 samDesired, u64* phkResult);
    static PAPAYA_MS_ABI long  hle_reg_open_key_ex_w(u64 hKey, const wchar_t* lpSubKey, u32 ulOptions, u32 samDesired, u64* phkResult);
    static PAPAYA_MS_ABI long  hle_reg_set_value_ex_a(u64 hKey, const char* lpValueName, u32 reserved, u32 dwType, const u8* lpData, u32 cbData);
    static PAPAYA_MS_ABI long  hle_reg_set_value_ex_w(u64 hKey, const wchar_t* lpValueName, u32 reserved, u32 dwType, const u8* lpData, u32 cbData);
    static PAPAYA_MS_ABI long  hle_reg_query_value_ex_a(u64 hKey, const char* lpValueName, u32 reserved, u32* lpType, u8* lpData, u32* lpcbData);
    static PAPAYA_MS_ABI long  hle_reg_query_value_ex_w(u64 hKey, const wchar_t* lpValueName, u32 reserved, u32* lpType, u8* lpData, u32* lpcbData);
    static PAPAYA_MS_ABI long  hle_reg_close_key(u64 hKey);
    static PAPAYA_MS_ABI long  hle_reg_delete_value_a(u64 hKey, const char* lpValueName);
    static PAPAYA_MS_ABI long  hle_reg_enum_value_a(u64 hKey, u32 dwIndex, char* lpName, u32* lpcchName, u32* lpType, u8* lpData, u32* lpcbData);
    static PAPAYA_MS_ABI long  hle_reg_enum_value_w(u64 hKey, u32 dwIndex, wchar_t* lpName, u32* lpcchName, u32* lpType, u8* lpData, u32* lpcbData);
    static PAPAYA_MS_ABI long  hle_reg_get_value_a(u64 hKey, const char* lpSubKey, const char* lpValue, u32 dwFlags, u32* pdwType, u8* pvData, u32* pcbData);
    static PAPAYA_MS_ABI void  hle_reg_disable_predefined_cache();
    static PAPAYA_MS_ABI u32   hle_set_error_mode(u32 uMode);
    static PAPAYA_MS_ABI void  hle_raise_exception(u32 code, u32 flags, u32 nargs, const u64* args);
    // Hardware profile + ShellExecute + drag-drop + IME (game-startup probes)
    static PAPAYA_MS_ABI BOOL  hle_get_current_hw_profile_a(void* pProfile);
    static PAPAYA_MS_ABI s64   hle_shell_execute_w(void* hwnd, const wchar_t* verb, const wchar_t* file, const wchar_t* params, const wchar_t* dir, int show);
    static PAPAYA_MS_ABI void  hle_drag_accept_files(void* hwnd, BOOL accept);
    static PAPAYA_MS_ABI u32   hle_drag_query_file_w(void* hdrop, u32 ifile, wchar_t* lpsz, u32 cch);
    static PAPAYA_MS_ABI void* hle_imm_get_context(void* hwnd);
    static PAPAYA_MS_ABI BOOL  hle_imm_release_context(void* hwnd, void* himc);
    static PAPAYA_MS_ABI s64   hle_imm_get_composition_string_w(void* himc, u32 index, void* buf, u32 buflen);
    static PAPAYA_MS_ABI void* hle_imm_associate_context(void* hwnd, void* himc);
    static PAPAYA_MS_ABI BOOL  hle_imm_set_candidate_window(void* himc, void* lpCandidateList);
    static PAPAYA_MS_ABI BOOL  hle_imm_set_composition_window(void* himc, void* lpCompositionForm);
    static PAPAYA_MS_ABI BOOL  hle_lookup_privilege_value_w(const wchar_t* lpSystemName, const wchar_t* lpName, u64* lpLuid);
    static PAPAYA_MS_ABI BOOL  hle_adjust_token_privileges(void* hToken, BOOL bDisableAllPrivileges, const void* lpNewState, u32 bufLen, void* lpPrevState, u32* lpReturnLength);
    static PAPAYA_MS_ABI u32   hle_sh_file_operation_w(const void* lpFileOp);

    // CRYPT32 certificate store (real, empty store for games that probe certs)
    static PAPAYA_MS_ABI void* hle_cert_open_system_store_a(void* hprov, const char* name);
    static PAPAYA_MS_ABI BOOL  hle_cert_close_store(void* store, u32 flags);
    static PAPAYA_MS_ABI void* hle_cert_enum_certificates_in_store(void* store, void* prev);
    static PAPAYA_MS_ABI BOOL  hle_cert_get_certificate_context_property(void* cert, u32 prop, void* data, void* len);
    static PAPAYA_MS_ABI BOOL  hle_crypt_binary_to_string_a(const u8* data, u32 len, u32 flags, char* str, u32* strlen);

    // WINMM
    static PAPAYA_MS_ABI u32   hle_time_get_time();
    static PAPAYA_MS_ABI u32   hle_time_begin_period(u32 uPeriod);
    static PAPAYA_MS_ABI u32   hle_time_end_period(u32 uPeriod);
    static PAPAYA_MS_ABI BOOL  hle_play_sound_a(const char* pszSound, void* hmod, u32 flags);
    static PAPAYA_MS_ABI BOOL  hle_play_sound_w(const wchar_t* pszSound, void* hmod, u32 flags);
    static PAPAYA_MS_ABI u32   hle_wave_out_get_num_devs();
    static PAPAYA_MS_ABI u32   hle_wave_out_open(u32* phwo, const void* pwfx, u32 cb, void* callbk, void* inst, u32 flags);
    static PAPAYA_MS_ABI u32   hle_wave_out_write(u32 hwo, const void* pwh, u32 cbwh);
    static PAPAYA_MS_ABI u32   hle_wave_out_close(u32 hwo);
    static PAPAYA_MS_ABI u32   hle_wave_out_set_volume(u32 hwo, u32 dwVolume);
    static PAPAYA_MS_ABI u32   hle_wave_in_get_num_devs();
    // mmsystem joystick + MCI + MIDI
    static PAPAYA_MS_ABI u32   hle_joy_get_num_devs();
    static PAPAYA_MS_ABI u32   hle_joy_get_pos_ex(u32 uJoyID, void* pji);
    static PAPAYA_MS_ABI u32   hle_joy_get_dev_caps_a(u32 uJoyID, void* pjc, u32 cbjc);
    static PAPAYA_MS_ABI u32   hle_mci_send_string_a(const char* lpCommand, char* lpRet, u32 cchRet, void* hwndCB);
    static PAPAYA_MS_ABI BOOL  hle_mci_get_error_string_a(u32 err, char* lpBuffer, u32 cchBuf);
    static PAPAYA_MS_ABI u32   hle_midi_out_short_msg(u32 hmo, u32 dwMsg);
    static PAPAYA_MS_ABI u32   hle_time_set_event(u32 delay, u32 resolution, void* func, void* arg, u32 evtype);

    // SHELL32
    static PAPAYA_MS_ABI s32   hle_sh_get_folder_path_a(HWND hwnd, int csidl, HANDLE hToken, u32 dwFlags, char* pszPath);
    static PAPAYA_MS_ABI s32   hle_sh_get_folder_path_w(HWND hwnd, int csidl, HANDLE hToken, u32 dwFlags, wchar_t* pszPath);
    static PAPAYA_MS_ABI s32   hle_sh_get_known_folder_path(const void* rfid, u32 dwFlags, HANDLE hToken, wchar_t** ppszPath);
    static PAPAYA_MS_ABI wchar_t** hle_command_line_to_argv_w(const wchar_t* lpCmdLine, int* pNumArgs);

    // OLE32 / OLEAUT32
    static PAPAYA_MS_ABI s32   hle_co_initialize(void* pvReserved);
    static PAPAYA_MS_ABI s32   hle_co_initialize_ex(void* pvReserved, u32 dwCoInit);
    static PAPAYA_MS_ABI void  hle_co_uninitialize();
    static PAPAYA_MS_ABI s32   hle_co_create_instance(const void* rclsid, void* pUnkOuter, u32 dwClsContext, const void* riid, void** ppv);
    static PAPAYA_MS_ABI void* hle_co_task_mem_alloc(size_t cb);
    static PAPAYA_MS_ABI void  hle_co_task_mem_free(void* pv);

    // GDI32
    static PAPAYA_MS_ABI int   hle_choose_pixel_format(void* hdc, const void* ppfd);
    static PAPAYA_MS_ABI BOOL  hle_set_pixel_format(void* hdc, int format, const void* ppfd);
    static PAPAYA_MS_ABI int   hle_describe_pixel_format(void* hdc, int iPixelFormat, u32 nBytes, void* ppfd);
    static PAPAYA_MS_ABI BOOL  hle_swap_buffers(void* hdc);
    static PAPAYA_MS_ABI void* hle_create_compatible_dc(void* hdc);
    static PAPAYA_MS_ABI void* hle_create_compatible_bitmap(void* hdc, int w, int h);
    static PAPAYA_MS_ABI void* hle_select_object(void* hdc, void* obj);
    static PAPAYA_MS_ABI BOOL  hle_delete_object(void* hobj);
    static PAPAYA_MS_ABI BOOL  hle_delete_dc(void* hdc);
    static PAPAYA_MS_ABI u32   hle_set_pixel(void* hdc, int x, int y, u32 color);
    static PAPAYA_MS_ABI BOOL  hle_bit_blt(void* dst_dc, int dx, int dy, int dw, int dh,
                                           void* src_dc, int sx, int sy, u32 rop);
    static PAPAYA_MS_ABI u32   hle_get_pixel(void* hdc, int x, int y);
    static PAPAYA_MS_ABI int   hle_get_device_caps(void* hdc, int nIndex);
    static PAPAYA_MS_ABI void* hle_get_stock_object(int fnObject);
    static PAPAYA_MS_ABI int   hle_get_object_a(void* h, int nCount, void* lpObject);
    static PAPAYA_MS_ABI u32   hle_set_bk_color(void* hdc, u32 crColor);
    static PAPAYA_MS_ABI u32   hle_set_text_color(void* hdc, u32 crColor);
    static PAPAYA_MS_ABI BOOL  hle_text_out_a(void* hdc, int x, int y, const char* lpString, int nCount);
    static PAPAYA_MS_ABI BOOL  hle_fill_rect(void* hdc, const void* lprc, void* hbr);
    static PAPAYA_MS_ABI BOOL  hle_rectangle(void* hdc, int l, int t, int r, int b);
    static PAPAYA_MS_ABI BOOL  hle_ellipse(void* hdc, int l, int t, int r, int b);
    static PAPAYA_MS_ABI BOOL  hle_move_to_ex(void* hdc, int x, int y, void* lpPoint);
    static PAPAYA_MS_ABI BOOL  hle_line_to(void* hdc, int x, int y);
    static PAPAYA_MS_ABI void* hle_create_pen(int style, int width, u32 color);
    static PAPAYA_MS_ABI void* hle_create_solid_brush(u32 color);
    static PAPAYA_MS_ABI BOOL  hle_get_class_name_a(HWND hWnd, char* lpClassName, int nMaxCount);
    static PAPAYA_MS_ABI BOOL  hle_set_bk_mode(void* hdc, int mode);
    static PAPAYA_MS_ABI u32   hle_set_text_align(void* hdc, u32 align);
    static PAPAYA_MS_ABI BOOL  hle_get_text_extent_point32_a(void* hdc, const char* lpString, int c, void* lpSize);
    static PAPAYA_MS_ABI BOOL  hle_get_text_metrics_a(void* hdc, void* lptm);
    static PAPAYA_MS_ABI BOOL  hle_draw_text_a(void* hdc, const char* lpChText, int cchText, void* lprc, u32 format);
    static PAPAYA_MS_ABI BOOL  hle_ext_text_out_a(void* hdc, int x, int y, u32 options, const void* lprc, const char* lpString, u32 c, const void* lpDx);
    static PAPAYA_MS_ABI BOOL  hle_enum_windows(void* lpEnumFunc, void* lParam);
    static PAPAYA_MS_ABI u32   hle_get_double_click_time();
    static PAPAYA_MS_ABI int   hle_get_keyboard_type(u32 nTypeFlag);
    static PAPAYA_MS_ABI u32   hle_time_get_dev_caps(void* caps, u32 size);
    static PAPAYA_MS_ABI void* hle_set_timer(HWND hWnd, int nIDEvent, u32 uElapse, void* lpTimerFunc);
    static PAPAYA_MS_ABI BOOL  hle_kill_timer(HWND hWnd, int uIDEvent);
    static PAPAYA_MS_ABI void* hle_monitor_from_window(HWND hwnd, u32 dwFlags);
    static PAPAYA_MS_ABI BOOL  hle_get_monitor_info_a(void* hMonitor, void* lpmi);
    static PAPAYA_MS_ABI BOOL  hle_enum_display_monitors(void* hdc, void* lpRect, void* lpProc, void* lParam);
    static PAPAYA_MS_ABI char* hle_lstrcat_a(char* dst, const char* src);
    static PAPAYA_MS_ABI int   hle_lstrlen_a(const char* str);
    static PAPAYA_MS_ABI u32   hle_wcslen(const void* str);
    static PAPAYA_MS_ABI u32   hle_get_system_default_lang_id();
    static PAPAYA_MS_ABI u32   hle_get_user_default_lang_id();
    static PAPAYA_MS_ABI u32   hle_get_process_id(void* hProcess);
    static PAPAYA_MS_ABI u32   hle_get_thread_locale(u32 dwFlags);
    static PAPAYA_MS_ABI BOOL  hle_get_handle_information(void* hObject, u32* lpdwFlags);
    static PAPAYA_MS_ABI void  hle_secure_zero_memory(void* pv, u64 cb);
    static PAPAYA_MS_ABI int   hle_get_dibits(void* hdc, void* hbm, u32 start, u32 clines, void* bits,
                                              const void* lpbmi, u32 usage);

    // DXGI & D3D11 software surface
    static PAPAYA_MS_ABI long   hle_d3d11_create_device(void* adapter, u32 driver, void* swrast, u32 flags,
                                                        const void* feature_levels, u32 nlev, u32 sdk,
                                                        void** device_out, void* feature_out, void** ctx_out);
    static PAPAYA_MS_ABI long   hle_d3d11_create_device_and_swapchain(void* adapter, u32 driver, void* swrast, u32 flags,
                                                                      const void* feature_levels, u32 nlev, u32 sdk,
                                                                      void* swapchain_desc, void** swapchain_out,
                                                                      void** device_out, void* feature_out, void** ctx_out);
    static PAPAYA_MS_ABI long   hle_create_dxgi_factory(void* riid, void** factory_out);

    // DirectSound (audio)
    static PAPAYA_MS_ABI long   hle_direct_sound_create(const void* guid, void** ods8_out, void* unk_outer);
    static PAPAYA_MS_ABI long   hle_direct_sound_create8(const void* guid, void** ods8_out, void* unk_outer);
    static PAPAYA_MS_ABI long   hle_direct_sound_enumerate_a(void* cb, void* ctx);

    // DirectInput8 (raw keyboard/mouse input)
    static PAPAYA_MS_ABI long   hle_direct_input8_create(void* hinst, u32 version, const void* iid, void** di8_out, void* unk_outer);
    static PAPAYA_MS_ABI long   hle_direct_input_create_a(void* hinst, u32 version, const void* iid, void** pdid_out, void* unk_outer);

    // OpenGL & Vulkan dynamic loaders
    static PAPAYA_MS_ABI void* hle_wgl_get_proc_address(const char* lpszProc);
    static PAPAYA_MS_ABI void* hle_wgl_create_context(void* hdc);
    static PAPAYA_MS_ABI int   hle_wgl_make_current(void* hdc, void* hglrc);
    static PAPAYA_MS_ABI int   hle_wgl_delete_context(void* hglrc);
    static PAPAYA_MS_ABI void* hle_vk_get_instance_proc_addr(void* instance, const char* pName);

    // Windows Version & Time
    static PAPAYA_MS_ABI u32   hle_get_version();
    static PAPAYA_MS_ABI BOOL  hle_get_version_ex_a(void* lpVersionInfo);
    static PAPAYA_MS_ABI BOOL  hle_get_version_ex_w(void* lpVersionInfo);
    static PAPAYA_MS_ABI void  hle_get_system_time_as_file_time(void* lpSystemTimeAsFileTime);
    static PAPAYA_MS_ABI void* hle_load_library_ex_a(const char* lpLibFileName, HANDLE hFile, u32 dwFlags);
    static PAPAYA_MS_ABI void* hle_load_library_ex_w(const wchar_t* lpLibFileName, HANDLE hFile, u32 dwFlags);
    static PAPAYA_MS_ABI u32   hle_get_module_file_name_w(void* hModule, wchar_t* lpFilename, u32 nSize);

    // Winsock (WS2_32)
    static PAPAYA_MS_ABI int   hle_wsa_startup(u16 wVersionRequested, void* lpWSAData);
    static PAPAYA_MS_ABI int   hle_wsa_cleanup();
    static PAPAYA_MS_ABI int   hle_wsa_get_last_error();
    static PAPAYA_MS_ABI u64   hle_socket(int af, int type, int protocol);
    static PAPAYA_MS_ABI int   hle_closesocket(u64 s);
    static PAPAYA_MS_ABI int   hle_connect(u64 s, const void* name, int namelen);
    static PAPAYA_MS_ABI int   hle_wsaconnect(u64 s, const void* name, int namelen, const void* lpCallerData, void* lpCalleeData, const void* lpSQOS, const void* lpGQOS);
    static PAPAYA_MS_ABI int   hle_send(u64 s, const char* buf, int len, int flags);
    static PAPAYA_MS_ABI int   hle_recv(u64 s, char* buf, int len, int flags);
    static PAPAYA_MS_ABI u16   hle_htons(u16 hostshort);
    static PAPAYA_MS_ABI u32   hle_htonl(u32 hostlong);
    static PAPAYA_MS_ABI u16   hle_ntohs(u16 netshort);
    static PAPAYA_MS_ABI u32   hle_ntohl(u32 netlong);
    static PAPAYA_MS_ABI int   hle_bind(u64 s, const void* addr, int addrlen);
    static PAPAYA_MS_ABI int   hle_listen(u64 s, int backlog);
    static PAPAYA_MS_ABI u64   hle_accept(u64 s, void* addr, void* addrlen_ptr);
    static PAPAYA_MS_ABI int   hle_getsockname(u64 s, void* name, void* namelen_ptr);
    static PAPAYA_MS_ABI int   hle_getpeername(u64 s, void* name, void* namelen_ptr);
    static PAPAYA_MS_ABI int   hle_setsockopt(u64 s, int level, int optname, const void* optval, int optlen);
    static PAPAYA_MS_ABI int   hle_shutdown(u64 s, int how);
    static PAPAYA_MS_ABI u32   hle_inet_addr(const char* cp);
    static PAPAYA_MS_ABI const char* hle_inet_ntoa(void* in_addr_ptr);
    static PAPAYA_MS_ABI int   hle_select(u32 nfds, void* rfds, void* wfds, void* efds, void* timeout);
    static PAPAYA_MS_ABI int   hle_getaddrinfo(const char* nodename, const char* servname, const void* hints, void** res);
    static PAPAYA_MS_ABI void  hle_freeaddrinfo(void* res);
    static PAPAYA_MS_ABI int   hle_getnameinfo(const void* sa, u32 salen, char* host, u32 hostlen, char* serv, u32 servlen, u32 flags);
    static PAPAYA_MS_ABI int   hle_inet_pton(int af, const char* src, void* dst);

    // USER32 Input & Window Additions
    static PAPAYA_MS_ABI BOOL  hle_get_cursor_pos(void* lpPoint);
    static PAPAYA_MS_ABI BOOL  hle_set_cursor_pos(int X, int Y);
    static PAPAYA_MS_ABI int   hle_show_cursor(BOOL bShow);
    static PAPAYA_MS_ABI s16   hle_get_async_key_state(int vKey);
    static PAPAYA_MS_ABI s16   hle_get_key_state(int vKey);
    static PAPAYA_MS_ABI BOOL  hle_get_keyboard_state(u8* lpKeyState);
    static PAPAYA_MS_ABI BOOL  hle_set_window_text_a(HWND hWnd, const char* lpString);
    static PAPAYA_MS_ABI BOOL  hle_set_window_text_w(HWND hWnd, const wchar_t* lpString);
    static PAPAYA_MS_ABI int   hle_get_window_text_a(HWND hWnd, char* lpString, int nMaxCount);
    static PAPAYA_MS_ABI int   hle_get_window_text_w(HWND hWnd, wchar_t* lpString, int nMaxCount);
    static PAPAYA_MS_ABI BOOL  hle_adjust_window_rect(void* lpRect, u32 dwStyle, BOOL bMenu);
    static PAPAYA_MS_ABI BOOL  hle_adjust_window_rect_ex(void* lpRect, u32 dwStyle, BOOL bMenu, u32 dwExStyle);
    static PAPAYA_MS_ABI BOOL  hle_set_window_pos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, u32 uFlags);

private:
    std::shared_ptr<steam::SteamApiStub> steam_stub_;
    std::shared_ptr<input::VirtualXInputManager> input_mgr_;
    std::unordered_map<std::string, std::unordered_map<std::string, void*>> export_table_;
};

} // namespace papaya::win32
