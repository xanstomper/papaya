#include "papaya/rom/android_jni.hpp"
#include "papaya/rom/rom_loader.hpp"
#include "papaya/rom/steam_rom_bridge.hpp"
#include "papaya/frontend/emulator_runtime.hpp"
#include "papaya/common/logger.hpp"

static std::unique_ptr<papaya::frontend::EmulatorRuntime> g_android_runtime;
static std::unique_ptr<papaya::rom::RomImageLoader> g_rom_loader;

extern "C" {

int papaya_jni_init_runtime(int potato_mode, int device_tier) {
    papaya::log::info("JNI", "Android NDK JNI: Initializing Papaya Runtime (Potato: {}, Tier: {})", potato_mode, device_tier);

    papaya::frontend::RuntimeConfig cfg{};
    cfg.force_potato_mode = (potato_mode != 0);
    cfg.headless = false;

    g_android_runtime = std::make_unique<papaya::frontend::EmulatorRuntime>(cfg);
    auto res = g_android_runtime->initialize();
    return res.has_value() ? 0 : -1;
}

int papaya_jni_load_rom_fd(int fd, long total_size_bytes) {
    papaya::log::info("JNI", "Android NDK JNI: Loading ROM from File Descriptor {} (Size: {} MB)", fd, total_size_bytes / papaya::MiB);

    g_rom_loader = std::make_unique<papaya::rom::RomImageLoader>();
    auto meta_res = g_rom_loader->open_descriptor(fd, static_cast<papaya::u64>(total_size_bytes));
    if (!meta_res) {
        return -1;
    }

    if (g_android_runtime) {
        papaya::rom::SteamRomBridge::bind_rom_to_steam_stub(*meta_res, "android_rom.iso", g_android_runtime->get_steam());
        g_android_runtime->launch_game("android_rom.iso");
    }

    return 0;
}

void papaya_jni_step_frame() {
    if (g_android_runtime) {
        g_android_runtime->step_frame();
    }
}

void papaya_jni_send_touch_input(int slot, int buttons, int axis_x, int axis_y) {
    if (g_android_runtime) {
        papaya::input::VirtualGamepadState state{};
        state.buttons = static_cast<papaya::u16>(buttons);
        state.thumb_lx = static_cast<papaya::s16>(axis_x);
        state.thumb_ly = static_cast<papaya::s16>(axis_y);
        g_android_runtime->get_input().set_pad_state(static_cast<papaya::u32>(slot), state);
    }
}

void papaya_jni_shutdown() {
    if (g_android_runtime) {
        g_android_runtime->stop();
        g_android_runtime.reset();
    }
    if (g_rom_loader) {
        g_rom_loader->close();
        g_rom_loader.reset();
    }
    papaya::log::info("JNI", "Android NDK JNI: Runtime shut down cleanly");
}

}
