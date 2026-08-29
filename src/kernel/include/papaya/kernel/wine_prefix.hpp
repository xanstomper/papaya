#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <filesystem>
#include <string>

namespace papaya::kernel {

struct WinePrefixConfig {
    std::filesystem::path prefix_root{"./papaya_prefix"};
    std::string user_name{"steamuser"};
    bool use_proot_container{false};
};

class WinePrefixManager {
public:
    explicit WinePrefixManager(const WinePrefixConfig& config = {});
    ~WinePrefixManager() = default;

    Result<> initialize_prefix();

    std::filesystem::path get_drive_c() const;
    std::filesystem::path get_appdata_roaming() const;
    std::filesystem::path get_appdata_local() const;
    std::filesystem::path get_steam_install_path() const;

    // Translates Windows file path ("C:\Program Files\Game\game.exe") to host POSIX path
    std::filesystem::path resolve_windows_path(std::string_view win_path) const;

private:
    WinePrefixConfig config_;
};

} // namespace papaya::kernel
