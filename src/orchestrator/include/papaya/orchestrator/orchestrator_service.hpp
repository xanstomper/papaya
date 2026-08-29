#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <string>
#include <string_view>
#include <filesystem>
#include <vector>

namespace papaya::orchestrator {

struct DeployedGameInfo {
    std::string title_name;
    std::string game_id;
    std::filesystem::path staging_dir;
    std::filesystem::path prefix_dir;
    std::filesystem::path save_vault_dir;
    std::filesystem::path desktop_shortcut;
    bool potato_mode_enabled{true};
    u32 fps_limit{60};
};

class OrchestratorService {
public:
    explicit OrchestratorService(const std::filesystem::path& papaya_root = "./Papaya");
    ~OrchestratorService() = default;

    // Ingest a dropped file (ISO, ZIP, EXE) and execute the 7-phase deployment pipeline
    Result<DeployedGameInfo> deploy_game(const std::filesystem::path& input_file);

    // Queries game compatibility database for title
    bool query_compat_profile(std::string_view game_id, u32& out_fps_limit, f32& out_lod_bias);

    // Creates isolated Wine prefix sandbox
    Result<std::filesystem::path> build_isolated_prefix(std::string_view game_id);

    // Injects DXVK Potato Mode and GPU spoofing config
    Result<> inject_dxvk_config(const std::filesystem::path& target_game_dir, std::string_view game_id);

    // Stubs Steamworks DRM (Goldberg)
    Result<> stub_steamworks_drm(const std::filesystem::path& target_game_dir, std::string_view game_id);

    // Links save directories to central Save Vault
    Result<> link_save_vault(const std::filesystem::path& prefix_dir, std::string_view game_id, std::string_view title_name);

    // Generates desktop & Steam shortcuts
    Result<std::filesystem::path> create_shortcuts(std::string_view game_id, std::string_view title_name, const std::filesystem::path& target_exe);

private:
    std::filesystem::path papaya_root_;
    std::filesystem::path watch_dir_;
    std::filesystem::path prefixes_dir_;
    std::filesystem::path staging_dir_;
    std::filesystem::path saves_dir_;
    std::filesystem::path shortcuts_dir_;
};

} // namespace papaya::orchestrator
