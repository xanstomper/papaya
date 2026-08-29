#include "papaya/common/logger.hpp"
#include "papaya/frontend/emulator_runtime.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace papaya;
    using namespace papaya::frontend;

    log::info("TEST", "Running unit test: test_emulator_runtime");

    EmulatorConfig config{
        .target = ConsoleTarget::PlayStation4,
        .backend = PlatformBackend::Kvm,
        .headless = true,
        .target_fps = 60,
        .boot_title_path = ""
    };

    EmulatorRuntime runtime(config);
    assert(runtime.initialize().has_value());

    // Verify all subsystems were instantiated properly
    assert(runtime.get_hypervisor().is_initialized());
    assert(runtime.get_window().is_headless());
    assert(runtime.get_audio().is_streaming());

    // Test stepping a frame
    runtime.step_frame();
    assert(runtime.get_frame_count() == 0); // No active title, stays 0

    runtime.stop();
    assert(!runtime.get_audio().is_streaming());

    log::info("TEST", ">>> test_emulator_runtime PASSED ALL CHECKS! <<<");
    return 0;
}
