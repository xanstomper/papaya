#include "papaya/frontend/emulator_runtime.hpp"
#include "papaya/hv/kvm/kvm_long_mode.hpp"
#include "papaya/common/logger.hpp"
#include <chrono>
#include <thread>

namespace papaya::frontend {

EmulatorRuntime::EmulatorRuntime(const EmulatorConfig& config)
    : config_(config) {}

EmulatorRuntime::~EmulatorRuntime() {
    stop();
}

Result<> EmulatorRuntime::initialize() {
    log::info("RUNTIME", "Initializing Papaya PlayStation 4 & PlayStation 5 Emulator Core...");

    // 1. Virtual File System
    vfs_ = std::make_shared<storage::VirtualFileSystem>();
    vfs_->setup_playstation_mounts("./app0");

    // 2. Physical Memory Map (8GB Unified for PS4 / 16GB Unified for PS5)
    memory_map_ = std::make_unique<hv::MemoryMap>();
    if (!memory_map_->initialize_playstation_layout(config_.target)) {
        log::error("RUNTIME", "Failed to setup PlayStation unified memory map");
        return ErrorCode::MemoryMappingFailed;
    }

    // 3. Hardware Hypervisor / Execution Context (Linux KVM)
    hv_ = hv::create_hypervisor(config_.backend);
    if (!hv_ || !hv_->initialize()) {
        log::error("RUNTIME", "Hypervisor initialization failed");
        return ErrorCode::HypervisorInitFailed;
    }
    if (!hv_->configure_memory(*memory_map_)) {
        log::error("RUNTIME", "Memory registration with hypervisor failed");
        return ErrorCode::MemoryMappingFailed;
    }

    // 4. Identity Map 64-bit Long Mode Page Tables and TEB/PEB
    void* host_ram = memory_map_->get_host_pointer(0x0);
    u64 total_ram = memory_map_->get_total_ram_size();
    if (!hv::kvm::KvmLongMode::initialize_page_tables(host_ram, total_ram)) {
        log::error("RUNTIME", "Failed to initialize 64-bit page tables");
        return ErrorCode::HypervisorInitFailed;
    }
    if (!hv::kvm::KvmLongMode::initialize_teb_peb(host_ram, total_ram)) {
        log::error("RUNTIME", "Failed to initialize TEB/PEB");
        return ErrorCode::HypervisorInitFailed;
    }

    // 5. Audio Engine (3D Tempest Audio & AudioOut)
    audio_ = std::make_unique<audio::AudioEngine>();
    if (!audio_->initialize()) {
        log::error("RUNTIME", "Audio engine initialization failed");
        return ErrorCode::AudioInitFailed;
    }
    audio_->start_stream();

    // 6. Input Subsystem (DualShock 4 & DualSense)
    input_ = std::make_unique<input::InputManager>();
    if (!input_->initialize()) {
        log::error("RUNTIME", "Input subsystem initialization failed");
        return ErrorCode::InvalidParameter;
    }

    // 7. PlayStation HLE Kernel (FreeBSD 9/12 + PRX shims)
    kernel_ = std::make_unique<hle::Kernel>(hv_, vfs_, input_.get(), audio_.get(), config_.target);
    if (!kernel_->initialize(host_ram, total_ram)) {
        log::error("RUNTIME", "PlayStation HLE Kernel initialization failed");
        return ErrorCode::UnsupportedOperation;
    }

    // 8. GPU Subsystem (Vulkan 1.3 GCN/RDNA2 Dynamic Renderer)
    gpu_ = std::make_unique<gpu::GpuCore>();
    if (!gpu_->initialize()) {
        log::error("RUNTIME", "GPU core initialization failed");
        return ErrorCode::GpuInitFailed;
    }

    // 9. Window Manager
    WindowConfig win_cfg{
        .title = (config_.target == ConsoleTarget::PlayStation5)
                 ? "Project Papaya - PlayStation 5 (Prospero OS) Emulator"
                 : "Project Papaya - PlayStation 4 (Orbis OS) Emulator",
        .width = 1920,
        .height = 1080,
        .fullscreen = false,
        .headless = config_.headless,
        .vsync = true
    };
    window_ = std::make_unique<WindowManager>(win_cfg);
    if (!window_->initialize()) {
        log::error("RUNTIME", "Window initialization failed");
        return ErrorCode::InvalidParameter;
    }

    log::info("RUNTIME", "All Papaya PS4/PS5 subsystems successfully initialized!");
    return {};
}

Result<> EmulatorRuntime::boot_title(std::string_view eboot_path) {
    if (!kernel_ || !memory_map_ || !hv_) {
        return ErrorCode::InvalidParameter;
    }

    void* host_ram = memory_map_->get_host_pointer(0x0);
    u64 total_ram = memory_map_->get_total_ram_size();

    auto load_res = kernel_->load_title_executable(eboot_path, host_ram, total_ram);
    if (!load_res) {
        log::error("RUNTIME", "Failed to load PlayStation ELF executable: {}", eboot_path);
        return load_res.error();
    }

    log::info("RUNTIME", "PlayStation ELF loaded: Base=0x{:X}, Entry=0x{:X}, Target={}",
              load_res->base_address, load_res->entry_point,
              load_res->is_ps5 ? "PlayStation 5" : "PlayStation 4");

    // Create and boot vCPU #0 in 64-bit Long Mode
    auto vcpu_res = hv_->create_vcpu(0);
    if (!vcpu_res) {
        log::error("RUNTIME", "Failed to create Primary vCPU #0");
        return vcpu_res.error();
    }

    primary_vcpu_ = *vcpu_res;
    constexpr GuestVirtAddr STACK_RSP = 0x00080000;
    if (!primary_vcpu_->setup_long_mode(load_res->entry_point, STACK_RSP)) {
        log::error("RUNTIME", "Failed to switch Primary vCPU #0 to Long Mode");
        return ErrorCode::VcpuRunFailed;
    }

    is_running_ = true;
    log::info("RUNTIME", "Primary vCPU #0 booted at entry point 0x{:X}", load_res->entry_point);
    return {};
}

void EmulatorRuntime::step_frame() {
    if (!primary_vcpu_ || !is_running_) return;

    window_->poll_events();
    if (window_->should_close()) {
        is_running_ = false;
        return;
    }

    // Run vCPU for slice
    void* host_ram = memory_map_->get_host_pointer(0x0);
    u64 total_ram = memory_map_->get_total_ram_size();

    for (int step = 0; step < 100 && is_running_; ++step) {
        auto exit_res = primary_vcpu_->run_once();
        if (!exit_res) {
            is_running_ = false;
            break;
        }

        const auto& exit = *exit_res;
        if (exit.reason == hv::ExitReason::IoOut) {
            if (exit.address == hle::HLE_HYPERCALL_IO_PORT) {
                u32 thunk_id = static_cast<u32>(exit.data & 0xFFFFFFFF);
                kernel_->get_thunk_manager().execute_thunk(thunk_id, *primary_vcpu_, host_ram, total_ram);
            }
        } else if (exit.reason == hv::ExitReason::Halt) {
            is_running_ = false;
            log::info("RUNTIME", "PlayStation title execution halted cleanly.");
            break;
        }
    }

    frame_count_++;
}

void EmulatorRuntime::run() {
    is_running_ = true;
    auto last_time = std::chrono::steady_clock::now();
    u64 last_frame = 0;

    while (is_running_ && !window_->should_close()) {
        step_frame();

        auto now = std::chrono::steady_clock::now();
        auto elapsed_s = std::chrono::duration<f64>(now - last_time).count();
        if (elapsed_s >= 1.0) {
            u64 cur_frame = frame_count_.load();
            current_fps_ = static_cast<f64>(cur_frame - last_frame) / elapsed_s;
            last_frame = cur_frame;
            last_time = now;
        }
    }
}

void EmulatorRuntime::stop() {
    is_running_ = false;
    if (audio_) {
        audio_->stop_stream();
    }
}

} // namespace papaya::frontend
