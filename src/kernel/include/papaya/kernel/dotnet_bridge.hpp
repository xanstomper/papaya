#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <memory>
#include <string>

namespace papaya::kernel {

// Selectively-native .NET bridge: locate + drive the host Common Language
// Runtime (libhostfxr) so a Godot-Mono / managed-assembly game boots via the
// real runtime instead of papaya reimplementing a CLR. This is the Tier-3
// "run the game's own runtime" path for .NET titles (e.g. SlayTheSpire2).
//
// Groundwork scope: hostfxr discovery + initialize_for_dotnet_command_line +
// hostfxr_run_app. Games that also need GodotSharp/.NET host glue on top are
// future extensions.
class DotnetBridge {
public:
    DotnetBridge();
    ~DotnetBridge();
    DotnetBridge(const DotnetBridge&) = delete;
    DotnetBridge& operator=(const DotnetBridge&) = delete;

    // Discover + dlopen the host hostfxr. Fails (UnsupportedOperation) when no
    // dotnet host is present — papaya then falls back to other paths.
    Result<> load();
    void shutdown();

    bool loaded() const { return loaded_; }
    const std::string& hostfxr_path() const { return hostfxr_path_; }
    const std::string& last_error() const { return last_error_; }

    // Boot a managed app via the host runtime.
    Result<> run_app(const std::string& managed_app_abs, int* exit_code);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool loaded_{false};
    std::string hostfxr_path_;
    std::string last_error_;
};

} // namespace papaya::kernel