#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <span>
#include <filesystem>
#include <chrono>

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

struct SteamRemoteFile {
    std::string filename;
    std::vector<u8> data;
    u64 timestamp{0};
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

    // ISteamUser Emulation
    u64 get_user_steam_id() const { return config_.steam_id; }
    bool is_logged_on() const { return true; }
    std::filesystem::path get_user_data_folder() const;

    // ISteamFriends Emulation
    std::string_view get_persona_name() const { return config_.player_name; }
    u32 get_friend_count() const { return 0; }

    // ISteamUtils Emulation
    u32 get_seconds_since_app_active() const;
    std::string_view get_ip_country() const { return "US"; }

    // ISteamUserStats Emulation
    bool set_achievement(std::string_view name);
    bool get_achievement(std::string_view name, bool& achieved) const;
    bool clear_achievement(std::string_view name);
    bool set_stat(std::string_view name, s32 value);
    bool get_stat(std::string_view name, s32& value) const;
    bool set_stat_float(std::string_view name, f32 value);
    bool get_stat_float(std::string_view name, f32& value) const;
    bool store_stats();
    bool reset_all_stats(bool achievements_too = false);

    // ISteamApps Emulation
    bool is_dlc_installed(u32 dlc_app_id) const;
    bool is_subscribed_app(u32 app_id) const;
    std::string get_current_game_language() const { return config_.language; }
    std::string get_available_game_languages() const { return "english,french,german,spanish,japanese,schinese"; }

    // ISteamRemoteStorage Emulation (Cloud Saves Simulation)
    bool file_write(std::string_view filename, std::span<const u8> data);
    bool file_read(std::string_view filename, std::vector<u8>& out_data) const;
    bool file_exists(std::string_view filename) const;
    s32 get_file_size(std::string_view filename) const;
    s32 get_file_count() const;
    std::string get_file_name_and_size(s32 index, s32& out_file_size) const;
    bool file_delete(std::string_view filename);

    // ISteamInput Emulation
    bool input_init();
    bool input_shutdown();
    u32 get_connected_controllers(u64* handles_out) const;

    // Direct C-ABI Export Resolver
    void* resolve_export(std::string_view symbol_name);

private:
    void load_stats_from_disk();
    void save_stats_to_disk();

    SteamProfileConfig config_;
    bool is_initialized_{false};
    std::chrono::steady_clock::time_point start_time_;

    std::unordered_set<std::string> unlocked_achievements_;
    std::unordered_map<std::string, s32> int_stats_;
    std::unordered_map<std::string, f32> float_stats_;
    std::unordered_set<u32> installed_dlcs_;
    std::unordered_map<std::string, SteamRemoteFile> remote_storage_files_;
};

} // namespace papaya::steam
