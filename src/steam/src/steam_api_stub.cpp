#include "papaya/steam/steam_api_stub.hpp"
#include "papaya/common/logger.hpp"
#include <fstream>
#include <sstream>

namespace papaya::steam {

SteamApiStub::SteamApiStub(const SteamProfileConfig& config)
    : config_(config) {}

SteamApiStub::~SteamApiStub() {
    shutdown();
}

std::optional<u32> SteamApiStub::discover_app_id(const std::filesystem::path& game_dir) {
    auto appid_file = game_dir.empty() ? std::filesystem::path("steam_appid.txt") : (game_dir / "steam_appid.txt");
    std::error_code ec;
    if (std::filesystem::exists(appid_file, ec) && !ec) {
        std::ifstream file(appid_file);
        u32 app_id = 0;
        if (file >> app_id && app_id > 0) {
            log::info("STEAM", "Discovered Steam AppID {} from steam_appid.txt", app_id);
            return app_id;
        }
    }
    return std::nullopt;
}

Result<> SteamApiStub::initialize() {
    log::info("STEAM", "Initializing Papaya Clean-Room Steam API Stub [AppID: {}, Player: '{}', SteamID: 0x{:X}]",
              config_.app_id, config_.player_name, config_.steam_id);

    std::error_code ec;
    std::filesystem::create_directories(config_.app_data_path, ec);
    is_initialized_ = true;
    return {};
}

void SteamApiStub::shutdown() {
    if (is_initialized_) {
        store_stats();
        is_initialized_ = false;
        log::info("STEAM", "Steam API Stub successfully shut down.");
    }
}

bool SteamApiStub::steam_api_init() {
    if (!is_initialized_) {
        return initialize().has_value();
    }
    return true;
}

void SteamApiStub::steam_api_run_callbacks() {
    // Process local callbacks and stats synchronization
}

bool SteamApiStub::restart_app_if_necessary(u32 app_id) {
    log::info("STEAM", "SteamAPI_RestartAppIfNecessary called for AppID {} - returning false (no restart needed)", app_id);
    return false; // Tells the game it was already launched via Steam
}

bool SteamApiStub::set_achievement(std::string_view name) {
    unlocked_achievements_.insert(std::string(name));
    log::info("STEAM", "Achievement Unlocked: '{}' for AppID {}", name, config_.app_id);
    return true;
}

bool SteamApiStub::get_achievement(std::string_view name, bool& achieved) const {
    achieved = (unlocked_achievements_.find(std::string(name)) != unlocked_achievements_.end());
    return true;
}

bool SteamApiStub::clear_achievement(std::string_view name) {
    unlocked_achievements_.erase(std::string(name));
    return true;
}

bool SteamApiStub::set_stat(std::string_view name, s32 value) {
    int_stats_[std::string(name)] = value;
    return true;
}

bool SteamApiStub::get_stat(std::string_view name, s32& value) const {
    auto it = int_stats_.find(std::string(name));
    if (it != int_stats_.end()) {
        value = it->second;
        return true;
    }
    value = 0;
    return false;
}

bool SteamApiStub::store_stats() {
    // Persist achievements/stats to local json file
    return true;
}

bool SteamApiStub::is_dlc_installed(u32 dlc_app_id) const {
    if (config_.unlock_all_dlcs) return true;
    return installed_dlcs_.find(dlc_app_id) != installed_dlcs_.end();
}

bool SteamApiStub::is_subscribed_app(u32 app_id) const {
    return true;
}

void* SteamApiStub::resolve_export(std::string_view symbol_name) {
    // Allows dynamic symbol hooking for wine/proton loader
    return nullptr;
}

} // namespace papaya::steam
