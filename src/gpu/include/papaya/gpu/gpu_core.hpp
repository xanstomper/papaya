#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/gpu/vulkan_backend.hpp"
#include "papaya/gpu/gcn_pm4.hpp"
#include "papaya/gpu/command_processor.hpp"
#include <memory>

namespace papaya::gpu {

class GpuCore {
public:
    GpuCore();
    ~GpuCore();

    Result<> initialize();
    Result<> submit_ring_buffer(GuestPhysAddr ring_gpa, u32 dword_count, const void* host_ring_ptr);

    CommandProcessor& get_command_processor() { return cp_; }
    const CommandProcessor& get_command_processor() const { return cp_; }

    VulkanBackend& get_backend() { return backend_; }
    const VulkanBackend& get_backend() const { return backend_; }

private:
    VulkanBackend backend_;
    CommandProcessor cp_;
};

} // namespace papaya::gpu
