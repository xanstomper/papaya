#include "papaya/gpu/gpu_core.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::gpu {

GpuCore::GpuCore() = default;
GpuCore::~GpuCore() = default;

Result<> GpuCore::initialize() {
    log::info("GPU", "Initializing Papaya GPU Subsystem (AMD GCN/RDNA)");
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

    return backend_.execute_pm4_packets(packets);
}

} // namespace papaya::gpu
