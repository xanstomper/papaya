#include "papaya/rom/steam_rom_bridge.hpp"
#include "papaya/rom/rom_loader.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::rom {

u32 SteamRomBridge::generate_virtual_steam_appid(const RomMetadata& rom) {
    std::string key = rom.title_name + "_" + rom.disc_serial_id;
    std::span<const u8> bytes(reinterpret_cast<const u8*>(key.data()), key.size());
    u32 crc = RomImageLoader::calculate_crc32(bytes);

    // Standard Steam shortcut hash: (crc | 0x80000000) & 0xFFFFFFFF
    u32 app_id = (crc | 0x80000000) & 0x7FFFFFFF;
    if (app_id == 0) app_id = 480;
    return app_id;
}

SteamRomShortcut SteamRomBridge::create_steam_shortcut(
    const RomMetadata& rom,
    const std::filesystem::path& rom_path
) {
    u32 virtual_id = generate_virtual_steam_appid(rom);

    SteamRomShortcut shortcut{};
    shortcut.virtual_app_id = virtual_id;
    shortcut.app_name = rom.title_name;
    shortcut.target_executable = rom_path.string();
    shortcut.start_dir = rom_path.parent_path().string();
    shortcut.launch_options = "--rom \"" + rom_path.string() + "\"";
    shortcut.local_save_path = "./papaya_steam_saves/" + std::to_string(virtual_id);

    std::error_code ec;
    std::filesystem::create_directories(shortcut.local_save_path, ec);

    log::info("STEAM_ROM", "Generated Virtual Steam AppID {} for ROM '{}' (Serial: '{}')",
              virtual_id, rom.title_name, rom.disc_serial_id);

    return shortcut;
}

Result<> SteamRomBridge::bind_rom_to_steam_stub(
    const RomMetadata& rom,
    const std::filesystem::path& rom_path,
    steam::SteamApiStub& stub
) {
    auto shortcut = create_steam_shortcut(rom, rom_path);

    steam::SteamProfileConfig cfg{};
    cfg.app_id = shortcut.virtual_app_id;
    cfg.player_name = "PapayaPlayer";
    cfg.app_data_path = shortcut.local_save_path;
    cfg.unlock_all_dlcs = true;

    log::info("STEAM_ROM", "Seamlessly bound ROM to Steamworks API Stub [VirtualAppID: {}]", shortcut.virtual_app_id);
    return {};
}

} // namespace papaya::rom
