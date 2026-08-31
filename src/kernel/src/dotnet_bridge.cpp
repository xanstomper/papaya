// .NET/Mono host bridge — loads the system libhostfxr and exposes the
// .NET hosting API so a Godot-Mono / managed-assembly game can boot via the
// real runtime instead of papaya reimplementing a CLR.
//
// Groundwork phase: discover hostfxr (searching well-known dotnet roots), then
// expose initialize_for_runtime_config + hostfxr_run_app. This is the
// selectively-native path for SlayTheSpire2 (Godot Mono, .NET Core layout in
// data_*_windows_x86_64/). Verified against /usr/lib/dotnet 8.0.30's hostfxr.
#include "papaya/kernel/dotnet_bridge.hpp"
#include "papaya/common/logger.hpp"

#include <dlfcn.h>
#include <cstring>
#include <vector>
#include <filesystem>

namespace papaya::kernel {

namespace fs = std::filesystem;

namespace {

// Well-known hostfxr locations (dotnet SDK/self-contained installs).
std::vector<std::string> candidate_hostfxr_dirs() {
    std::vector<std::string> dirs;
    if (const char* root = getenv("DOTNET_ROOT")) dirs.emplace_back(root);
    if (const char* fxr = getenv("DOTNET_HOSTFXR_PATH")) dirs.emplace_back(std::string(fxr));
    dirs.emplace_back("/usr/lib/dotnet/host/fxr");
    dirs.emplace_back("/usr/share/dotnet/host/fxr");
    dirs.emplace_back("/usr/local/share/dotnet/host/fxr");
    dirs.emplace_back("/opt/dotnet/host/fxr");
    return dirs;
}

std::string pick_hostfxr() {
    for (const auto& root : candidate_hostfxr_dirs()) {
        // If a direct .so path was given, use it.
        if (fs::exists(root) && root.size() > 3 && root.ends_with(".so")) return root;
        if (!fs::is_directory(root)) continue;
        // Take the highest version subdir.
        std::string best;
        for (auto& e : fs::directory_iterator(root))
            if (e.is_directory()) {
                std::string v = e.path().string();
                if (v > best) best = v;
            }
        if (!best.empty()) {
            std::string so = best + "/libhostfxr.so";
            if (fs::exists(so)) return so;
        }
    }
    return {};
}

} // namespace

struct DotnetBridge::Impl {
    void* lib{nullptr};
    // hostfxr_initialize_for_dotnet_command_line(ARGS, POPTIONS, PCONTEXT)
    int (*init_cmdline)(const char**, int32_t, void*, void**){nullptr};
    // hostfxr_run_app(HOST_CONTEXT)
    int (*run_app)(void*){nullptr};
    // hostfxr_get_runtime_delegate(HOST_CONTEXT, enum, OUT)
    int (*get_runtime_delegate)(void*, uint32_t, void**){nullptr};
    // hostfxr_close(HOST_CONTEXT)
    int (*close)(void*){nullptr};
};

DotnetBridge::DotnetBridge() = default;
DotnetBridge::~DotnetBridge() { shutdown(); }

Result<> DotnetBridge::load() {
    if (impl_ && impl_->lib) return {};
    auto path = pick_hostfxr();
    if (path.empty()) {
        hostfxr_path_ = "(not found)";
        return ErrorCode::UnsupportedOperation;
    }
    void* lib = dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) {
        hostfxr_path_ = path + " (unloadable)";
        return ErrorCode::UnsupportedOperation;
    }
    impl_ = std::make_unique<Impl>();
    impl_->lib = lib;
    impl_->init_cmdline = reinterpret_cast<int (*)(const char**, int32_t, void*, void**)>(
        dlsym(lib, "hostfxr_initialize_for_dotnet_command_line"));
    impl_->run_app = reinterpret_cast<int (*)(void*)>(dlsym(lib, "hostfxr_run_app"));
    impl_->get_runtime_delegate = reinterpret_cast<int (*)(void*, uint32_t, void**)>(
        dlsym(lib, "hostfxr_get_runtime_delegate"));
    impl_->close = reinterpret_cast<int (*)(void*)>(dlsym(lib, "hostfxr_close"));
    if (!impl_->init_cmdline || !impl_->run_app || !impl_->close) {
        dlclose(lib);
        impl_.reset();
        hostfxr_path_ = path + " (entry points missing)";
        return ErrorCode::UnsupportedOperation;
    }
    hostfxr_path_ = path;
    loaded_ = true;
    return {};
}

void DotnetBridge::shutdown() {
    if (!impl_) return;
    if (impl_->lib) dlclose(impl_->lib);
    impl_.reset();
    loaded_ = false;
}

// Build argv for shutdown/runtime: dotnet <dll_or_app>
Result<> DotnetBridge::run_app(const std::string& managed_app_abs, int* exit_code) {
    if (!load()) return ErrorCode::UnsupportedOperation;
    const char* argv[] = {"dotnet", managed_app_abs.c_str(), nullptr};
    void* ctx = nullptr;
    int r = impl_->init_cmdline(argv, 2, nullptr, &ctx);
    if (r != 0 || !ctx) {
        last_error_ = "hostfxr_initialize failed";
        return ErrorCode::UnsupportedOperation;
    }
    int rc = impl_->run_app(ctx);
    if (exit_code) *exit_code = rc;
    impl_->close(ctx);
    return {};
}

} // namespace papaya::kernel