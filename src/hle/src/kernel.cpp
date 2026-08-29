#include "papaya/hle/kernel.hpp"
#include "papaya/common/logger.hpp"
#include <chrono>
#include <cstring>

namespace papaya::hle {

Kernel::Kernel(
    std::shared_ptr<hv::IHypervisor> hv,
    std::shared_ptr<storage::VirtualFileSystem> vfs,
    input::InputManager* input,
    audio::AudioEngine* audio
) : hv_(hv), vfs_(vfs), input_(input), audio_(audio),
    thunk_manager_(0x00100000), thread_manager_(handle_table_) {}

Kernel::~Kernel() = default;

Result<> Kernel::initialize() {
    log::info("KERNEL", "Initializing Papaya High-Level Emulation (HLE) Runtime & Win32 subsystem");

    register_kernel32_exports();
    register_ntdll_exports();
    register_xg_exports();
    register_synchronization_exports();
    register_xinput_exports();
    register_xaudio_exports();

    return {};
}

Result<storage::LoadedPeImage> Kernel::load_title_executable(
    std::string_view exe_path,
    void* guest_memory_host_base,
    u64 guest_memory_size
) {
    if (!vfs_) {
        return ErrorCode::InvalidParameter;
    }

    auto file_res = vfs_->read_file(exe_path);
    if (!file_res) {
        log::error("KERNEL", "Failed to read Title executable: {}", exe_path);
        return file_res.error();
    }

    log::info("KERNEL", "Loading title PE executable ({} bytes)", file_res->size());
    auto img_res = pe_loader_.load_image(*file_res, 0x00400000, guest_memory_host_base, guest_memory_size);
    if (!img_res) {
        return img_res.error();
    }

    // Write HLE trampolines to 0x00100000
    thunk_manager_.write_trampolines_to_guest(guest_memory_host_base, guest_memory_size);

    // Bind PE imports to trampolines
    auto bind_res = thunk_manager_.bind_imports(img_res->imports, guest_memory_host_base, guest_memory_size);
    if (!bind_res) {
        log::error("KERNEL", "Failed to bind imports for title executable");
        return bind_res.error();
    }

    return *img_res;
}

void Kernel::register_kernel32_exports() {
    thunk_manager_.register_function("kernel32.dll", "GetModuleHandleA", [](HleCallContext&) -> u64 {
        return 0x00400000;
    });

    thunk_manager_.register_function("kernel32.dll", "GetModuleHandleW", [](HleCallContext&) -> u64 {
        return 0x00400000;
    });

    thunk_manager_.register_function("kernel32.dll", "VirtualAlloc", [](HleCallContext& ctx) -> u64 {
        GuestVirtAddr lpAddress = ctx.rcx;
        u64 dwSize = ctx.rdx;
        if (lpAddress == 0) lpAddress = 0x10000000;
        log::debug("HLE", "VirtualAlloc(addr=0x{:X}, size=0x{:X})", lpAddress, dwSize);
        return lpAddress;
    });

    thunk_manager_.register_function("kernel32.dll", "VirtualFree", [](HleCallContext&) -> u64 {
        return 1; // Success
    });

    thunk_manager_.register_function("kernel32.dll", "VirtualProtect", [](HleCallContext& ctx) -> u64 {
        if (ctx.host_ram_base && ctx.r9 != 0) {
            auto* ram = static_cast<u8*>(ctx.host_ram_base);
            *reinterpret_cast<u32*>(ram + ctx.r9) = 0x04; // PAGE_READWRITE
        }
        return 1;
    });

    thunk_manager_.register_function("kernel32.dll", "HeapCreate", [](HleCallContext&) -> u64 {
        return 0x18000000; // Heap handle
    });

    thunk_manager_.register_function("kernel32.dll", "HeapDestroy", [](HleCallContext&) -> u64 {
        return 1;
    });

    thunk_manager_.register_function("kernel32.dll", "HeapAlloc", [](HleCallContext& ctx) -> u64 {
        u64 dwBytes = ctx.r8;
        static GuestVirtAddr heap_ptr = 0x18001000;
        GuestVirtAddr addr = heap_ptr;
        heap_ptr += ((dwBytes + 15) & ~15);
        return addr;
    });

    thunk_manager_.register_function("kernel32.dll", "HeapFree", [](HleCallContext&) -> u64 {
        return 1;
    });

    thunk_manager_.register_function("kernel32.dll", "QueryPerformanceCounter", [](HleCallContext& ctx) -> u64 {
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
        u64 counts = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        if (ctx.rcx != 0 && ctx.host_ram_base) {
            auto* ram = static_cast<u8*>(ctx.host_ram_base);
            *reinterpret_cast<u64*>(ram + ctx.rcx) = counts;
        }
        return 1;
    });

    thunk_manager_.register_function("kernel32.dll", "QueryPerformanceFrequency", [](HleCallContext& ctx) -> u64 {
        if (ctx.rcx != 0 && ctx.host_ram_base) {
            auto* ram = static_cast<u8*>(ctx.host_ram_base);
            *reinterpret_cast<u64*>(ram + ctx.rcx) = 1000000000ULL; // 1 GHz
        }
        return 1;
    });

    thunk_manager_.register_function("kernel32.dll", "OutputDebugStringA", [](HleCallContext& ctx) -> u64 {
        if (ctx.rcx != 0 && ctx.host_ram_base) {
            auto* ram = static_cast<u8*>(ctx.host_ram_base);
            const char* str = reinterpret_cast<const char*>(ram + ctx.rcx);
            log::info("GUEST_DEBUG", "{}", str);
        }
        return 0;
    });
}

void Kernel::register_ntdll_exports() {
    thunk_manager_.register_function("ntdll.dll", "RtlAllocateHeap", [](HleCallContext& ctx) -> u64 {
        u64 size = ctx.r8;
        static GuestVirtAddr heap_alloc_ptr = 0x18000000;
        GuestVirtAddr addr = heap_alloc_ptr;
        heap_alloc_ptr += ((size + 15) & ~15);
        return addr;
    });

    thunk_manager_.register_function("ntdll.dll", "RtlFreeHeap", [](HleCallContext&) -> u64 {
        return 1;
    });

    thunk_manager_.register_function("ntdll.dll", "RtlZeroMemory", [](HleCallContext& ctx) -> u64 {
        if (ctx.host_ram_base && ctx.rcx != 0) {
            auto* ram = static_cast<u8*>(ctx.host_ram_base);
            std::memset(ram + ctx.rcx, 0, ctx.rdx);
        }
        return 0;
    });

    thunk_manager_.register_function("ntdll.dll", "RtlCopyMemory", [](HleCallContext& ctx) -> u64 {
        if (ctx.host_ram_base && ctx.rcx != 0 && ctx.rdx != 0) {
            auto* ram = static_cast<u8*>(ctx.host_ram_base);
            std::memcpy(ram + ctx.rcx, ram + ctx.rdx, ctx.r8);
        }
        return 0;
    });
}

void Kernel::register_xg_exports() {
    thunk_manager_.register_function("xg.dll", "XgCreateDevice", [](HleCallContext&) -> u64 {
        log::info("XG", "XgCreateDevice invoked by guest");
        return 0; // S_OK
    });

    thunk_manager_.register_function("xg.dll", "XgSubmitCommandRing", [](HleCallContext& ctx) -> u64 {
        GuestVirtAddr ring_gpa = ctx.rcx;
        u32 dword_count = static_cast<u32>(ctx.rdx);
        log::debug("XG", "XgSubmitCommandRing(ring_gpa=0x{:X}, dwords={})", ring_gpa, dword_count);
        return 0; // S_OK
    });
}

void Kernel::register_synchronization_exports() {
    // 1. Events
    thunk_manager_.register_function("kernel32.dll", "CreateEventA", [this](HleCallContext& ctx) -> u64 {
        bool manual_reset = (ctx.rdx != 0);
        bool initial_state = (ctx.r8 != 0);
        auto evt = std::make_shared<HleEvent>(manual_reset, initial_state);
        u32 handle = handle_table_.insert(evt);
        return handle;
    });

    thunk_manager_.register_function("kernel32.dll", "CreateEventW", [this](HleCallContext& ctx) -> u64 {
        bool manual_reset = (ctx.rdx != 0);
        bool initial_state = (ctx.r8 != 0);
        auto evt = std::make_shared<HleEvent>(manual_reset, initial_state);
        return handle_table_.insert(evt);
    });

    thunk_manager_.register_function("kernel32.dll", "SetEvent", [this](HleCallContext& ctx) -> u64 {
        auto obj = handle_table_.get(static_cast<u32>(ctx.rcx));
        if (obj && obj->get_type() == HandleType::Event) {
            static_cast<HleEvent*>(obj.get())->set();
            return 1;
        }
        return 0;
    });

    thunk_manager_.register_function("kernel32.dll", "ResetEvent", [this](HleCallContext& ctx) -> u64 {
        auto obj = handle_table_.get(static_cast<u32>(ctx.rcx));
        if (obj && obj->get_type() == HandleType::Event) {
            static_cast<HleEvent*>(obj.get())->reset();
            return 1;
        }
        return 0;
    });

    // 2. Mutexes
    thunk_manager_.register_function("kernel32.dll", "CreateMutexA", [this](HleCallContext& ctx) -> u64 {
        bool initial_owner = (ctx.rdx != 0);
        auto mtx = std::make_shared<HleMutex>(initial_owner, thread_manager_.get_current_tid());
        return handle_table_.insert(mtx);
    });

    thunk_manager_.register_function("kernel32.dll", "CreateMutexW", [this](HleCallContext& ctx) -> u64 {
        bool initial_owner = (ctx.rdx != 0);
        auto mtx = std::make_shared<HleMutex>(initial_owner, thread_manager_.get_current_tid());
        return handle_table_.insert(mtx);
    });

    thunk_manager_.register_function("kernel32.dll", "ReleaseMutex", [this](HleCallContext& ctx) -> u64 {
        auto obj = handle_table_.get(static_cast<u32>(ctx.rcx));
        if (obj && obj->get_type() == HandleType::Mutex) {
            return static_cast<HleMutex*>(obj.get())->release(thread_manager_.get_current_tid()) ? 1 : 0;
        }
        return 0;
    });

    // 3. Semaphores
    thunk_manager_.register_function("kernel32.dll", "CreateSemaphoreA", [this](HleCallContext& ctx) -> u64 {
        s32 initial_count = static_cast<s32>(ctx.rdx);
        s32 max_count = static_cast<s32>(ctx.r8);
        auto sem = std::make_shared<HleSemaphore>(initial_count, max_count);
        return handle_table_.insert(sem);
    });

    thunk_manager_.register_function("kernel32.dll", "ReleaseSemaphore", [this](HleCallContext& ctx) -> u64 {
        auto obj = handle_table_.get(static_cast<u32>(ctx.rcx));
        if (obj && obj->get_type() == HandleType::Semaphore) {
            s32 release_count = static_cast<s32>(ctx.rdx);
            s32* prev_ptr = nullptr;
            if (ctx.r8 != 0 && ctx.host_ram_base) {
                auto* ram = static_cast<u8*>(ctx.host_ram_base);
                prev_ptr = reinterpret_cast<s32*>(ram + ctx.r8);
            }
            return static_cast<HleSemaphore*>(obj.get())->release(release_count, prev_ptr) ? 1 : 0;
        }
        return 0;
    });

    // 4. Waiting
    thunk_manager_.register_function("kernel32.dll", "WaitForSingleObject", [this](HleCallContext& ctx) -> u64 {
        u32 handle = static_cast<u32>(ctx.rcx);
        u32 timeout_ms = static_cast<u32>(ctx.rdx);
        return handle_table_.wait_for_single_object(handle, timeout_ms);
    });

    thunk_manager_.register_function("kernel32.dll", "CloseHandle", [this](HleCallContext& ctx) -> u64 {
        u32 handle = static_cast<u32>(ctx.rcx);
        return handle_table_.remove(handle) ? 1 : 0;
    });

    // 5. Fast Futex Synchronization (WaitOnAddress / WakeByAddress)
    thunk_manager_.register_function("api-ms-win-core-synch-l1-2-0.dll", "WaitOnAddress", [](HleCallContext& ctx) -> u64 {
        if (!ctx.host_ram_base || ctx.rcx == 0 || ctx.rdx == 0) return 0;
        auto* ram = static_cast<u8*>(ctx.host_ram_base);
        void* addr = ram + ctx.rcx;
        u64 compare_val = *reinterpret_cast<const u64*>(ram + ctx.rdx);
        size_t size = ctx.r8;
        u32 timeout_ms = static_cast<u32>(ctx.r9);
        return HleFutex::wait_on_address(addr, compare_val, size, timeout_ms) ? 1 : 0;
    });

    thunk_manager_.register_function("api-ms-win-core-synch-l1-2-0.dll", "WakeByAddressSingle", [](HleCallContext& ctx) -> u64 {
        if (!ctx.host_ram_base || ctx.rcx == 0) return 0;
        auto* ram = static_cast<u8*>(ctx.host_ram_base);
        void* addr = ram + ctx.rcx;
        HleFutex::wake_by_address_single(addr);
        return 1;
    });

    thunk_manager_.register_function("api-ms-win-core-synch-l1-2-0.dll", "WakeByAddressAll", [](HleCallContext& ctx) -> u64 {
        if (!ctx.host_ram_base || ctx.rcx == 0) return 0;
        auto* ram = static_cast<u8*>(ctx.host_ram_base);
        void* addr = ram + ctx.rcx;
        HleFutex::wake_by_address_all(addr);
        return 1;
    });

    // 6. Thread Management
    thunk_manager_.register_function("kernel32.dll", "CreateThread", [this](HleCallContext& ctx) -> u64 {
        u64 stack_size = ctx.rdx;
        GuestVirtAddr start_address = ctx.r8;
        GuestVirtAddr parameter = ctx.r9;
        auto res = thread_manager_.create_thread(start_address, parameter, stack_size, 0);
        return res.has_value() ? *res : 0;
    });

    thunk_manager_.register_function("kernel32.dll", "ResumeThread", [this](HleCallContext& ctx) -> u64 {
        return thread_manager_.resume_thread(static_cast<u32>(ctx.rcx)) ? 1 : 0;
    });

    thunk_manager_.register_function("kernel32.dll", "SuspendThread", [this](HleCallContext& ctx) -> u64 {
        return thread_manager_.suspend_thread(static_cast<u32>(ctx.rcx)) ? 0 : static_cast<u64>(-1);
    });

    thunk_manager_.register_function("kernel32.dll", "ExitThread", [this](HleCallContext& ctx) -> u64 {
        thread_manager_.exit_thread(static_cast<u32>(ctx.rcx));
        return 0;
    });

    thunk_manager_.register_function("kernel32.dll", "GetCurrentThreadId", [this](HleCallContext&) -> u64 {
        return thread_manager_.get_current_tid();
    });

    thunk_manager_.register_function("kernel32.dll", "GetCurrentProcessId", [](HleCallContext&) -> u64 {
        return 0x1337;
    });

    // 7. Thread Local Storage (TLS)
    thunk_manager_.register_function("kernel32.dll", "TlsAlloc", [this](HleCallContext&) -> u64 {
        return thread_manager_.tls_alloc();
    });

    thunk_manager_.register_function("kernel32.dll", "TlsFree", [this](HleCallContext& ctx) -> u64 {
        return thread_manager_.tls_free(static_cast<u32>(ctx.rcx)) ? 1 : 0;
    });

    thunk_manager_.register_function("kernel32.dll", "TlsGetValue", [this](HleCallContext& ctx) -> u64 {
        return thread_manager_.tls_get_value(static_cast<u32>(ctx.rcx), thread_manager_.get_current_tid());
    });

    thunk_manager_.register_function("kernel32.dll", "TlsSetValue", [this](HleCallContext& ctx) -> u64 {
        return thread_manager_.tls_set_value(static_cast<u32>(ctx.rcx), ctx.rdx, thread_manager_.get_current_tid()) ? 1 : 0;
    });
}

void Kernel::register_xinput_exports() {
    thunk_manager_.register_function("xinput1_4.dll", "XInputGetState", [this](HleCallContext& ctx) -> u64 {
        u32 user_index = static_cast<u32>(ctx.rcx);
        GuestVirtAddr pState = ctx.rdx;

        if (user_index >= input::MAX_CONTROLLERS || pState == 0 || !ctx.host_ram_base) {
            return 1167; // ERROR_DEVICE_NOT_CONNECTED
        }

        if (input_) {
            auto* ram = static_cast<u8*>(ctx.host_ram_base);
            const auto& state = input_->get_gamepad_state(user_index);
            // XINPUT_STATE: dwPacketNumber (u32), Gamepad (GamepadState)
            *reinterpret_cast<u32*>(ram + pState) = 1; // dwPacketNumber
            std::memcpy(ram + pState + sizeof(u32), &state, sizeof(input::GamepadState));
            return 0; // ERROR_SUCCESS
        }

        return 0;
    });

    thunk_manager_.register_function("xinput1_4.dll", "XInputSetState", [this](HleCallContext& ctx) -> u64 {
        u32 user_index = static_cast<u32>(ctx.rcx);
        GuestVirtAddr pVib = ctx.rdx;

        if (user_index >= input::MAX_CONTROLLERS || pVib == 0 || !ctx.host_ram_base) {
            return 1167;
        }

        if (input_) {
            auto* ram = static_cast<u8*>(ctx.host_ram_base);
            const auto* vib = reinterpret_cast<const input::GamepadVibration*>(ram + pVib);
            input_->set_vibration(user_index, *vib);
            return 0; // ERROR_SUCCESS
        }

        return 0;
    });
}

void Kernel::register_xaudio_exports() {
    thunk_manager_.register_function("xaudio2_8.dll", "XAudio2Create", [](HleCallContext& ctx) -> u64 {
        log::info("XAUDIO2", "XAudio2Create engine initialized");
        if (ctx.host_ram_base && ctx.rcx != 0) {
            auto* ram = static_cast<u8*>(ctx.host_ram_base);
            *reinterpret_cast<u64*>(ram + ctx.rcx) = 0x50000000ULL; // XAudio2 COM object interface pointer
        }
        return 0; // S_OK
    });
}

} // namespace papaya::hle
