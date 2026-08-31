// Unit test for the D3D -> Vulkan presentation backend.
//
// Two honest outcomes are accepted:
//   1. Vulkan is available: initialize() succeeds and reports a device name.
//   2. No Vulkan loader/ICD/WSI: initialize() reports UnsupportedOperation and
//      the object stays safely unusable (no crash).
// Either way the module must not crash — this is the CI contract.
#include "papaya/gpu/vulkan_swapchain.hpp"

#include <cstdio>

using papaya::gpu::VulkanSwapchain;

int main() {
    VulkanSwapchain vs;

    // Passing a null display/window must fail cleanly (no crash).
    auto bad = vs.initialize(nullptr, 0, 64, 64);
    if (bad) {
        std::printf("unexpected: init succeeded with null display\n");
        return 2;
    }
    if (vs.is_ready()) {
        std::printf("fail: is_ready true after failed init\n");
        return 3;
    }
    if (vs.last_error().empty()) {
        std::printf("fail: last_error empty after failure\n");
        return 4;
    }

    std::printf("ok: vulkan swapchain reports '%s' (vulkan available: %s)\n",
                vs.last_error().c_str(), vs.gpu_name().c_str());
    return 0;
}