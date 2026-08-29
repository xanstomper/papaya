#include "papaya/storage/vfs.hpp"
#include "papaya/common/logger.hpp"
#include <fstream>

namespace papaya::storage {

class HostFileNode : public IVfsNode {
public:
    explicit HostFileNode(std::filesystem::path path) : path_(std::move(path)) {}

    std::string_view get_name() const override {
        name_cache_ = path_.filename().string();
        return name_cache_;
    }

    bool is_directory() const override {
        return std::filesystem::is_directory(path_);
    }

    u64 get_size() const override {
        if (!std::filesystem::exists(path_) || is_directory()) return 0;
        return std::filesystem::file_size(path_);
    }

    Result<std::vector<u8>> read_all() override {
        if (!std::filesystem::exists(path_)) {
            return ErrorCode::FileNotFound;
        }

        std::ifstream file(path_, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return ErrorCode::InvalidParameter;
        }

        auto size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<u8> buffer(size);
        if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            return buffer;
        }

        return ErrorCode::InvalidParameter;
    }

private:
    std::filesystem::path path_;
    mutable std::string name_cache_;
};

VirtualFileSystem::VirtualFileSystem() = default;
VirtualFileSystem::~VirtualFileSystem() = default;

Result<> VirtualFileSystem::mount(std::string_view mount_point, const std::filesystem::path& host_path) {
    std::string mp(mount_point);
    if (!mp.ends_with('/')) mp += '/';

    host_mounts_[mp] = host_path;
    log::info("VFS", "Mounted virtual path '{}' -> host path '{}'", mp, host_path.string());
    return {};
}

Result<> VirtualFileSystem::setup_playstation_mounts(
    const std::filesystem::path& app_dir,
    const std::filesystem::path& save_dir,
    const std::filesystem::path& temp_dir
) {
    mount("/app0/", app_dir);
    mount("/savedata0/", save_dir);
    mount("/temp0/", temp_dir);
    mount("/hostapp/", app_dir);
    mount("/system/common/lib/", app_dir / "sce_module");
    log::info("VFS", "Configured PlayStation standard filesystem mounting hierarchy");
    return {};
}

std::shared_ptr<IVfsNode> VirtualFileSystem::resolve(std::string_view virtual_path) const {
    for (const auto& [mount_point, host_path] : host_mounts_) {
        if (virtual_path.starts_with(mount_point)) {
            auto relative_subpath = virtual_path.substr(mount_point.length());
            auto resolved_host_path = host_path / std::filesystem::path(relative_subpath);
            return std::make_shared<HostFileNode>(resolved_host_path);
        }
    }

    // Direct host path fallback
    std::filesystem::path host_path(virtual_path);
    if (std::filesystem::exists(host_path)) {
        return std::make_shared<HostFileNode>(host_path);
    }

    return nullptr;
}

bool VirtualFileSystem::exists(std::string_view virtual_path) const {
    auto node = resolve(virtual_path);
    return node != nullptr;
}

Result<std::vector<u8>> VirtualFileSystem::read_file(std::string_view virtual_path) const {
    auto node = resolve(virtual_path);
    if (!node) {
        return ErrorCode::FileNotFound;
    }
    return node->read_all();
}

} // namespace papaya::storage
