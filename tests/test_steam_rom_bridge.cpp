#include "papaya/common/logger.hpp"
#include "papaya/rom/steam_rom_bridge.hpp"
#include "papaya/steam/steam_api_stub.hpp"
#include <iostream>
#include <cstdlib>

#define TEST_CHECK(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "FAILED: " #expr << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::abort(); \
        } \
    } while (0)

int main() {
    using namespace papaya;
    using namespace papaya::rom;

    log::info("TEST", "Running unit test: test_steam_rom_bridge");

    RomMetadata rom{};
    rom.title_name = "GrandTheftAuto_SanAndreas";
    rom.disc_serial_id = "SLUS-20946";
    rom.format = RomFormat::Iso9660;

    // 1. Verify deterministic Virtual Steam AppID generation
    u32 virtual_id1 = SteamRomBridge::generate_virtual_steam_appid(rom);
    u32 virtual_id2 = SteamRomBridge::generate_virtual_steam_appid(rom);
    TEST_CHECK(virtual_id1 > 0);
    TEST_CHECK(virtual_id1 == virtual_id2);

    // 2. Verify Steam Shortcut Generation
    auto shortcut = SteamRomBridge::create_steam_shortcut(rom, "/games/ps2/gta_sa.iso");
    TEST_CHECK(shortcut.virtual_app_id == virtual_id1);
    TEST_CHECK(shortcut.app_name == "GrandTheftAuto_SanAndreas");
    TEST_CHECK(shortcut.target_executable == "/games/ps2/gta_sa.iso");
    TEST_CHECK(shortcut.launch_options.find("--rom") != std::string::npos);
    TEST_CHECK(std::filesystem::exists(shortcut.local_save_path));

    // 3. Verify binding to Steamworks API Stub
    steam::SteamProfileConfig scfg{};
    steam::SteamApiStub stub(scfg);
    TEST_CHECK(SteamRomBridge::bind_rom_to_steam_stub(rom, "/games/ps2/gta_sa.iso", stub).has_value());

    log::info("TEST", ">>> test_steam_rom_bridge PASSED ALL CHECKS! <<<");
    return 0;
}
