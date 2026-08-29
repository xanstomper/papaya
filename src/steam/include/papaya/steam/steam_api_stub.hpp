#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>

namespace papaya::steam {

// Local offline Steam ID representation
constexpr u64 DEFAULT_LOCAL_STEAM_ID = 76561198000000001ULL;

struct SteamProfileConfig {
    u32 app_id{480}; // Default SpaceWar (480)
    std::string player_name{"PapayaPlayer"};
    std::string language{"english"};
    u64 steam_id{DEFAULT_LOCAL_STEAM_ID};
    std::filesystem::path app_data_path{"./papaya_steam_saves"};
    bool unlock_all_dlcs{true};
};

class SteamApiStub {
public:
    explicit SteamApiStub(const SteamProfileConfig& config = {});
    ~SteamApiStub();

    Result<> initialize();
    void shutdown();

    bool is_initialized() const { return is_initialized_; }
    u32 get_app_id() const { return config_.app_id; }
    u64 get_steam_id() const { return config_.steam_id; }
    std::string_view get_player_name() const { return config_.player_name; }
    std::string_view get_language() const { return config_.language; }

    // App ID auto-discovery from steam_appid.txt or directory
    static std::optional<u32> discover_app_id(const std::filesystem::path& game_dir);

    // Steam Core Callbacks
    bool steam_api_init();
    void steam_api_run_callbacks();
    bool restart_app_if_necessary(u32 app_id);

    // ISteamUserStats Emulation
    bool set_achievement(std::string_view name);
    bool get_achievement(std::string_view name, bool& achieved) const;
    bool clear_achievement(std::string_view name);
    bool set_stat(std::string_view name, s32 value);
    bool get_stat(std::string_view name, s32& value) const;
    bool store_stats();

    // ISteamApps Emulation
    bool is_dlc_installed(u32 dlc_app_id) const;
    bool is_subscribed_app(u32 app_id) const;

    // Direct C-ABI Export Resolver
    void* resolve_export(std::string_view symbol_name);

private:
    SteamProfileConfig config_;
    bool is_initialized_{false};

    std::unordered_set<std::string> unlocked_achievements_;
    std::unordered_map<std::string, s32> int_stats_;
    std::unordered_set<u32> installed_dlcs_;
};

} // namespace papaya::steam
