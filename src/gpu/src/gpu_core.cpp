#include "papaya/gpu/gpu_core.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::gpu {

GpuCore::GpuCore() = default;
GpuCore::~GpuCore() = default;

Result<> GpuCore::initialize() {
    log::info("GPU", "Initializing Papaya GPU Subsystem (AMD GCN/RDNA & Command Processor)");

    // Wire draw callback to Vulkan backend
    cp_.set_draw_callback([this](const DrawCallRecord& draw) {
        log::debug("GPU", "Dispatching Draw to Vulkan: count={}, topology={}",
                   draw.count, static_cast<int>(draw.topology));
    });

    cp_.set_dispatch_callback([this](const DispatchCallRecord& dispatch) {
        log::debug("GPU", "Dispatching Compute to Vulkan: ({}, {}, {})",
                   dispatch.group_count_x, dispatch.group_count_y, dispatch.group_count_z);
    });

    return backend_.initialize();
}

Result<> GpuCore::submit_ring_buffer(GuestPhysAddr ring_gpa, u32 dword_count, const void* host_ring_ptr) {
    if (!host_ring_ptr || dword_count == 0) {
        return ErrorCode::InvalidParameter;
    }

    auto dword_span = std::span<const u32>(static_cast<const u32*>(host_ring_ptr), dword_count);
    auto packets = Pm4Parser::parse_stream(dword_span);

    log::debug("GPU", "Submitted ring buffer GPA 0x{:X}: {} dwords, parsed {} PM4 packets",
               ring_gpa, dword_count, packets.size());

    // 1. Process GCN register state updates & generate draw records
    auto cp_res = cp_.process_packets(packets);
    if (!cp_res) {
        return cp_res;
    }

    // 2. Execute low-level Vulkan pipeline commands
    return backend_.execute_pm4_packets(packets);
}

} // namespace papaya::gpu
