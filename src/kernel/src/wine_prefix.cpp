#include "papaya/kernel/wine_prefix.hpp"
#include "papaya/common/logger.hpp"
#include <algorithm>

namespace papaya::kernel {

WinePrefixManager::WinePrefixManager(const WinePrefixConfig& config)
    : config_(config) {}

Result<> WinePrefixManager::initialize_prefix() {
    auto drive_c = get_drive_c();
    std::filesystem::create_directories(drive_c / "windows" / "system32");
    std::filesystem::create_directories(drive_c / "Program Files (x86)" / "Steam");
    std::filesystem::create_directories(get_appdata_roaming());
    std::filesystem::create_directories(get_appdata_local());

    log::info("PREFIX", "Initialized Papaya Wine Prefix [Path: '{}', User: '{}']",
              config_.prefix_root.string(), config_.user_name);
    return {};
}

std::filesystem::path WinePrefixManager::get_drive_c() const {
    return config_.prefix_root / "drive_c";
}

std::filesystem::path WinePrefixManager::get_appdata_roaming() const {
    return get_drive_c() / "users" / config_.user_name / "AppData" / "Roaming";
}

std::filesystem::path WinePrefixManager::get_appdata_local() const {
    return get_drive_c() / "users" / config_.user_name / "AppData" / "Local";
}

std::filesystem::path WinePrefixManager::get_steam_install_path() const {
    return get_drive_c() / "Program Files (x86)" / "Steam";
}

std::filesystem::path WinePrefixManager::resolve_windows_path(std::string_view win_path) const {
    std::string path_str(win_path);
    std::replace(path_str.begin(), path_str.end(), '\\', '/');

    if (path_str.size() >= 2 && path_str[1] == ':') {
        char drive = static_cast<char>(std::tolower(path_str[0]));
        if (drive == 'c') {
            size_t start = (path_str.size() > 2 && path_str[2] == '/') ? 3 : 2;
            return get_drive_c() / path_str.substr(start);
        }
    }

    size_t start = (!path_str.empty() && path_str[0] == '/') ? 1 : 0;
    return get_drive_c() / path_str.substr(start);
}

} // namespace papaya::kernel
