#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/rom/rom_types.hpp"
#include "papaya/steam/steam_api_stub.hpp"
#include <filesystem>
#include <memory>

namespace papaya::rom {

struct SteamRomShortcut {
    u32 virtual_app_id{0};
    std::string app_name;
    std::string target_executable;
    std::string start_dir;
    std::string launch_options;
    std::string icon_path;
    std::string grid_cover_path;
    std::filesystem::path local_save_path;
};

class SteamRomBridge {
public:
    SteamRomBridge() = default;
    ~SteamRomBridge() = default;

    // Generates deterministic Virtual Steam AppID from ROM metadata
    static u32 generate_virtual_steam_appid(const RomMetadata& rom);

    // Creates complete Steam shortcut definition and saves folder
    static SteamRomShortcut create_steam_shortcut(const RomMetadata& rom, const std::filesystem::path& rom_path);

    // Binds a loaded ROM with the Steam API Stub
    static Result<> bind_rom_to_steam_stub(
        const RomMetadata& rom,
        const std::filesystem::path& rom_path,
        steam::SteamApiStub& stub
    );
};

} // namespace papaya::rom
