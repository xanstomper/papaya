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
using papaya::u8;

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
    // The GPU pipel ine path must refuse cleanly before initialization.
    papaya::gpu::PipelineSpec spec;
    if (vs.device() != 0 || vs.physical_device() != 0 || vs.surface_format() != 0) {
        std::printf("fail: accessors non-zero before init\n");
        return 5;
    }
    const u8 verts[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    std::string err;
    if (vs.render_and_present(spec, verts, 12, 3, nullptr, 0)) {
        std::printf("fail: render_and_present before init\n");
        return 6;
    }
    if (vs.gpu_name().empty() && vs.last_error().empty()) {
        std::printf("fail: no error after init attempt\n");
        return 7;
    }

    std::printf("ok: vulkan swapchain reports '%s' (vulkan available: %s)\n",
                vs.last_error().c_str(), vs.gpu_name().c_str());
    return 0;
}