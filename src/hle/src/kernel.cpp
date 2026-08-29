#include "papaya/hle/kernel.hpp"
#include "papaya/hle/sony_nids.hpp"
#include "papaya/common/logger.hpp"
#include <chrono>
#include <cstring>

namespace papaya::hle {

Kernel::Kernel(
    std::shared_ptr<hv::IHypervisor> hv,
    std::shared_ptr<storage::VirtualFileSystem> vfs,
    input::InputManager* input,
    audio::AudioEngine* audio,
    ConsoleTarget target
) : target_(target), hv_(hv), vfs_(vfs), input_(input), audio_(audio),
    thunk_manager_(0x00100000),
    thread_manager_(handle_table_),
    memory_manager_(target),
    syscall_dispatcher_(memory_manager_, thread_manager_) {}

Kernel::~Kernel() = default;

Result<> Kernel::initialize(void* guest_memory_host_base, u64 guest_memory_size) {
    log::info("KERNEL", "Initializing Papaya PlayStation (Orbis/Prospero) HLE Runtime Core");

    auto mem_res = memory_manager_.initialize(guest_memory_host_base, guest_memory_size);
    if (!mem_res) return mem_res;

    auto sc_res = syscall_dispatcher_.initialize();
    if (!sc_res) return sc_res;

    register_libkernel_exports();
    register_libscesysmodule_exports();
    register_libscesavedata_exports();
    register_libscepad_exports();
    register_libsceaudioout_exports();
    register_libsceagc_gnm_exports();
    register_libscefios2_exports();

    return {};
}

Result<storage::LoadedElfImage> Kernel::load_title_executable(
    std::string_view exe_path,
    void* guest_memory_host_base,
    u64 guest_memory_size
) {
    if (!vfs_) {
        return ErrorCode::InvalidParameter;
    }

    auto file_res = vfs_->read_file(exe_path);
    if (!file_res) {
        log::error("KERNEL", "Failed to read PlayStation ELF binary: {}", exe_path);
        return file_res.error();
    }

    log::info("KERNEL", "Loading PlayStation ELF64 executable ({} bytes)", file_res->size());
    auto img_res = elf_loader_.load_image(*file_res, 0x00400000, guest_memory_host_base, guest_memory_size);
    if (!img_res) {
        return img_res.error();
    }

    // Write HLE trampolines to 0x00100000
    thunk_manager_.write_trampolines_to_guest(guest_memory_host_base, guest_memory_size);

    return *img_res;
}

void Kernel::register_libkernel_exports() {
    // 1. Direct Memory
    thunk_manager_.register_function("libkernel.prx", "sceKernelAllocateDirectMemory", [this](HleCallContext& ctx) -> u64 {
        u64 search_start = ctx.rcx;
        u64 search_end   = ctx.rdx;
        u64 length       = ctx.r8;
        u64 alignment    = ctx.r9;
        // memory_type is passed on stack or in additional reg
        auto res = memory_manager_.allocate_direct_memory(search_start, search_end, length, alignment, SceMemoryType::MainCoherent);
        if (res.has_value()) {
            // Write phys_addr to out pointer (e.g. stack arg or r8)
            return 0; // SCE_OK
        }
        return 0x8002000C; // SCE_KERNEL_ERROR_NO_MEMORY
    });

    thunk_manager_.register_function("libkernel.prx", "sceKernelMapDirectMemory", [this](HleCallContext& ctx) -> u64 {
        GuestVirtAddr preferred = ctx.rcx;
        u64 length = ctx.rdx;
        u32 prot = static_cast<u32>(ctx.r8);
        u32 flags = static_cast<u32>(ctx.r9);
        auto res = memory_manager_.map_direct_memory(preferred, length, prot, flags, 0x80000000ULL, PAGE_SIZE_2M);
        return res.has_value() ? 0 : 0x8002000C;
    });

    thunk_manager_.register_function("libkernel.prx", "sceKernelGetDirectMemorySize", [this](HleCallContext&) -> u64 {
        return memory_manager_.get_total_ram_size();
    });

    // 2. Threading & Synchronization
    thunk_manager_.register_function("libkernel.prx", "scePthreadCreate", [this](HleCallContext& ctx) -> u64 {
        GuestVirtAddr thread_ptr = ctx.rcx;
        GuestVirtAddr entry = ctx.r8;
        GuestVirtAddr arg = ctx.r9;

        auto res = thread_manager_.create_thread(entry, arg, 1 * MiB, 0);
        if (res.has_value() && thread_ptr != 0 && ctx.host_ram_base) {
            auto* ram = static_cast<u8*>(ctx.host_ram_base);
            *reinterpret_cast<u64*>(ram + thread_ptr) = *res;
            return 0; // SCE_OK
        }
        return 0x80020001;
    });

    thunk_manager_.register_function("libkernel.prx", "scePthreadSelf", [this](HleCallContext&) -> u64 {
        return thread_manager_.get_current_tid();
    });

    thunk_manager_.register_function("libkernel.prx", "scePthreadExit", [this](HleCallContext&) -> u64 {
        thread_manager_.exit_thread(thread_manager_.get_current_tid());
        return 0;
    });

    thunk_manager_.register_function("libkernel.prx", "sceKernelCreateEventFlag", [this](HleCallContext& ctx) -> u64 {
        auto evt = std::make_shared<HleEvent>(true, false);
        u32 handle = handle_table_.insert(evt);
        if (ctx.host_ram_base && ctx.rcx != 0) {
            auto* ram = static_cast<u8*>(ctx.host_ram_base);
            *reinterpret_cast<u32*>(ram + ctx.rcx) = handle;
        }
        return 0; // SCE_OK
    });

    thunk_manager_.register_function("libkernel.prx", "sceKernelSetEventFlag", [this](HleCallContext& ctx) -> u64 {
        u32 handle = static_cast<u32>(ctx.rcx);
        auto obj = handle_table_.get(handle);
        if (obj && obj->get_type() == HandleType::Event) {
            static_cast<HleEvent*>(obj.get())->set();
            return 0;
        }
        return 0x80020009; // Invalid handle
    });

    thunk_manager_.register_function("libkernel.prx", "sceKernelWaitEventFlag", [this](HleCallContext& ctx) -> u64 {
        u32 handle = static_cast<u32>(ctx.rcx);
        return handle_table_.wait_for_single_object(handle, 1000) == 0 ? 0 : 0x8002000A;
    });

    thunk_manager_.register_function("libkernel.prx", "sceKernelUsleep", [](HleCallContext& ctx) -> u64 {
        u64 microseconds = ctx.rcx;
        log::trace("KERNEL", "sceKernelUsleep({} us)", microseconds);
        return 0;
    });

    thunk_manager_.register_function("libkernel.prx", "sceKernelGetProcessTime", [](HleCallContext&) -> u64 {
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    });
}

void Kernel::register_libscesysmodule_exports() {
    thunk_manager_.register_function("libSceSysmodule.prx", "sceSysmoduleLoadModule", [](HleCallContext& ctx) -> u64 {
        u32 moduleId = static_cast<u32>(ctx.rcx);
        log::info("SYSMODULE", "sceSysmoduleLoadModule(0x{:X}) -> Loaded", moduleId);
        return 0; // SCE_OK
    });

    thunk_manager_.register_function("libSceSysmodule.prx", "sceSysmoduleIsLoaded", [](HleCallContext&) -> u64 {
        return 0; // SCE_OK (Is loaded)
    });
}

void Kernel::register_libscesavedata_exports() {
    thunk_manager_.register_function("libSceSaveData.prx", "sceSaveDataInitialize3", [](HleCallContext&) -> u64 {
        log::info("SAVEDATA", "sceSaveDataInitialize3 initialized");
        return 0; // SCE_OK
    });
}

void Kernel::register_libscepad_exports() {
    thunk_manager_.register_function("libScePad.prx", "scePadInit", [](HleCallContext&) -> u64 {
        log::info("PAD", "scePadInit: Controller driver initialized (DualShock 4 / DualSense)");
        return 0; // SCE_OK
    });

    thunk_manager_.register_function("libScePad.prx", "scePadOpen", [](HleCallContext&) -> u64 {
        log::info("PAD", "scePadOpen: Port 0 opened");
        return 1; // Pad handle #1
    });

    thunk_manager_.register_function("libScePad.prx", "scePadReadState", [this](HleCallContext& ctx) -> u64 {
        GuestVirtAddr pData = ctx.rdx;
        if (input_ && ctx.host_ram_base && pData != 0) {
            auto* ram = static_cast<u8*>(ctx.host_ram_base);
            const auto& pad = input_->get_gamepad_state(0);
            // ScePadData: buttons (u32), lx (u8), ly (u8), rx (u8), ry (u8)
            *reinterpret_cast<u32*>(ram + pData) = pad.buttons;
            ram[pData + 4] = static_cast<u8>((pad.thumb_lx + 32768) >> 8);
            ram[pData + 5] = static_cast<u8>((pad.thumb_ly + 32768) >> 8);
            ram[pData + 6] = static_cast<u8>((pad.thumb_rx + 32768) >> 8);
            ram[pData + 7] = static_cast<u8>((pad.thumb_ry + 32768) >> 8);
        }
        return 0; // SCE_OK
    });
}

void Kernel::register_libsceaudioout_exports() {
    thunk_manager_.register_function("libSceAudioOut.prx", "sceAudioOutInit", [](HleCallContext&) -> u64 {
        log::info("AUDIOOUT", "sceAudioOutInit: 3D Tempest Audio & AudioOut initialized");
        return 0; // SCE_OK
    });

    thunk_manager_.register_function("libSceAudioOut.prx", "sceAudioOutOpen", [](HleCallContext&) -> u64 {
        return 1; // Audio port handle #1
    });

    thunk_manager_.register_function("libSceAudioOut.prx", "sceAudioOutOutput", [](HleCallContext&) -> u64 {
        return 0; // SCE_OK
    });
}

void Kernel::register_libsceagc_gnm_exports() {
    thunk_manager_.register_function("libSceAgc.prx", "sceAgcDraw", [](HleCallContext&) -> u64 {
        log::debug("AGC", "sceAgcDraw command submitted");
        return 0;
    });

    thunk_manager_.register_function("libSceGnmDriver.prx", "sceGnmSubmitCommandBuffers", [](HleCallContext& ctx) -> u64 {
        u32 count = static_cast<u32>(ctx.rcx);
        log::debug("GNM", "sceGnmSubmitCommandBuffers(count={})", count);
        return 0;
    });
}

void Kernel::register_libscefios2_exports() {
    thunk_manager_.register_function("libSceFios2.prx", "sceFiosInitialize", [](HleCallContext&) -> u64 {
        log::info("FIOS2", "sceFiosInitialize: High-speed async I/O streaming initialized");
        return 0;
    });
}

} // namespace papaya::hle
