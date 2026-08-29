#include "papaya/steam/steam_api_stub.hpp"
#include "papaya/common/logger.hpp"
#include <fstream>
#include <sstream>

namespace papaya::steam {

SteamApiStub::SteamApiStub(const SteamProfileConfig& config)
    : config_(config),
      start_time_(std::chrono::steady_clock::now()) {}

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
    std::filesystem::create_directories(get_user_data_folder(), ec);
    std::filesystem::create_directories(config_.app_data_path / "remotestorage", ec);

    load_stats_from_disk();
    is_initialized_ = true;
    start_time_ = std::chrono::steady_clock::now();
    return {};
}

void SteamApiStub::shutdown() {
    if (is_initialized_) {
        store_stats();
        is_initialized_ = false;
        log::info("STEAM", "Steam API Stub successfully shut down.");
    }
}

std::filesystem::path SteamApiStub::get_user_data_folder() const {
    return config_.app_data_path / "userdata" / std::to_string(config_.steam_id & 0xFFFFFFFF);
}

u32 SteamApiStub::get_seconds_since_app_active() const {
    auto now = std::chrono::steady_clock::now();
    return static_cast<u32>(std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count());
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

bool SteamApiStub::set_stat_float(std::string_view name, f32 value) {
    float_stats_[std::string(name)] = value;
    return true;
}

bool SteamApiStub::get_stat_float(std::string_view name, f32& value) const {
    auto it = float_stats_.find(std::string(name));
    if (it != float_stats_.end()) {
        value = it->second;
        return true;
    }
    value = 0.0f;
    return false;
}

bool SteamApiStub::store_stats() {
    save_stats_to_disk();
    return true;
}

bool SteamApiStub::reset_all_stats(bool achievements_too) {
    int_stats_.clear();
    float_stats_.clear();
    if (achievements_too) {
        unlocked_achievements_.clear();
    }
    store_stats();
    return true;
}

void SteamApiStub::load_stats_from_disk() {
    auto stats_file = config_.app_data_path / "stats.txt";
    std::ifstream file(stats_file);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("ACH:", 0) == 0) {
            unlocked_achievements_.insert(line.substr(4));
        } else if (line.rfind("STAT_INT:", 0) == 0) {
            auto pos = line.find('=', 9);
            if (pos != std::string::npos) {
                std::string k = line.substr(9, pos - 9);
                s32 v = std::stoi(line.substr(pos + 1));
                int_stats_[k] = v;
            }
        }
    }
    log::info("STEAM", "Loaded {} achievements and {} stats from disk.",
              unlocked_achievements_.size(), int_stats_.size());
}

void SteamApiStub::save_stats_to_disk() {
    auto stats_file = config_.app_data_path / "stats.txt";
    std::ofstream file(stats_file);
    if (!file.is_open()) return;

    for (const auto& ach : unlocked_achievements_) {
        file << "ACH:" << ach << "\n";
    }
    for (const auto& [k, v] : int_stats_) {
        file << "STAT_INT:" << k << "=" << v << "\n";
    }
    for (const auto& [k, v] : float_stats_) {
        file << "STAT_FLT:" << k << "=" << v << "\n";
    }
}

bool SteamApiStub::is_dlc_installed(u32 dlc_app_id) const {
    if (config_.unlock_all_dlcs) return true;
    return installed_dlcs_.find(dlc_app_id) != installed_dlcs_.end();
}

bool SteamApiStub::is_subscribed_app(u32 app_id) const {
    return true;
}

// ISteamRemoteStorage Emulation (Cloud Saves Simulation)
bool SteamApiStub::file_write(std::string_view filename, std::span<const u8> data) {
    auto storage_path = config_.app_data_path / "remotestorage" / filename;
    std::error_code ec;
    std::filesystem::create_directories(storage_path.parent_path(), ec);

    std::ofstream out(storage_path, std::ios::binary);
    if (!out.is_open()) return false;

    out.write(reinterpret_cast<const char*>(data.data()), data.size());

    SteamRemoteFile rf{};
    rf.filename = std::string(filename);
    rf.data.assign(data.begin(), data.end());
    rf.timestamp = static_cast<u64>(std::time(nullptr));
    remote_storage_files_[std::string(filename)] = std::move(rf);

    log::info("STEAM_CLOUD", "Wrote remote storage file '{}' ({} bytes)", filename, data.size());
    return true;
}

bool SteamApiStub::file_read(std::string_view filename, std::vector<u8>& out_data) const {
    auto storage_path = config_.app_data_path / "remotestorage" / filename;
    std::ifstream in(storage_path, std::ios::binary);
    if (!in.is_open()) return false;

    in.seekg(0, std::ios::end);
    size_t sz = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);

    out_data.resize(sz);
    in.read(reinterpret_cast<char*>(out_data.data()), sz);
    return true;
}

bool SteamApiStub::file_exists(std::string_view filename) const {
    auto storage_path = config_.app_data_path / "remotestorage" / filename;
    return std::filesystem::exists(storage_path);
}

s32 SteamApiStub::get_file_size(std::string_view filename) const {
    auto storage_path = config_.app_data_path / "remotestorage" / filename;
    std::error_code ec;
    auto sz = std::filesystem::file_size(storage_path, ec);
    return ec ? 0 : static_cast<s32>(sz);
}

s32 SteamApiStub::get_file_count() const {
    return static_cast<s32>(remote_storage_files_.size());
}

std::string SteamApiStub::get_file_name_and_size(s32 index, s32& out_file_size) const {
    s32 cur = 0;
    for (const auto& [name, file] : remote_storage_files_) {
        if (cur == index) {
            out_file_size = static_cast<s32>(file.data.size());
            return name;
        }
        cur++;
    }
    out_file_size = 0;
    return "";
}

bool SteamApiStub::file_delete(std::string_view filename) {
    auto storage_path = config_.app_data_path / "remotestorage" / filename;
    std::error_code ec;
    std::filesystem::remove(storage_path, ec);
    remote_storage_files_.erase(std::string(filename));
    return !ec;
}

bool SteamApiStub::input_init() { return true; }
bool SteamApiStub::input_shutdown() { return true; }
u32 SteamApiStub::get_connected_controllers(u64* handles_out) const {
    if (handles_out) handles_out[0] = 1;
    return 1;
}

void* SteamApiStub::resolve_export(std::string_view symbol_name) {
    return nullptr;
}

} // namespace papaya::steam
