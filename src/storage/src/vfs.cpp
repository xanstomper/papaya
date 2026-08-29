#include "papaya/storage/vfs.hpp"
#include "papaya/common/logger.hpp"
#include <fstream>

namespace papaya::storage {

class HostVfsNode : public IVfsNode {
public:
    explicit HostVfsNode(std::filesystem::path path) : path_(std::move(path)) {}

    std::string_view get_name() const override {
        name_cache_ = path_.filename().string();
        return name_cache_;
    }

    bool is_directory() const override {
        return std::filesystem::is_directory(path_);
    }

    u64 get_size() const override {
        if (is_directory()) return 0;
        return std::filesystem::file_size(path_);
    }

    Result<std::vector<u8>> read_all() override {
        if (is_directory()) {
            return ErrorCode::InvalidParameter;
        }
        std::ifstream f(path_, std::ios::binary);
        if (!f) {
            return ErrorCode::InvalidParameter;
        }
        f.seekg(0, std::ios::end);
        size_t sz = f.tellg();
        f.seekg(0, std::ios::beg);

        std::vector<u8> data(sz);
        f.read(reinterpret_cast<char*>(data.data()), sz);
        return data;
    }

private:
    std::filesystem::path path_;
    mutable std::string name_cache_;
};

VirtualFileSystem::VirtualFileSystem() = default;
VirtualFileSystem::~VirtualFileSystem() = default;

Result<> VirtualFileSystem::mount(std::string_view mount_point, const std::filesystem::path& host_path) {
    if (!std::filesystem::exists(host_path)) {
        log::error("VFS", "Host mount target does not exist: {}", host_path.string());
        return ErrorCode::InvalidParameter;
    }

    host_mounts_[std::string(mount_point)] = host_path;
    log::info("VFS", "Mounted '{}' -> '{}'", mount_point, host_path.string());
    return {};
}

Result<> VirtualFileSystem::mount_xvd(std::string_view mount_point, const std::filesystem::path& xvd_path) {
    log::info("VFS", "Mounting XVD image at '{}' -> '{}'", mount_point, xvd_path.string());
    return mount(mount_point, xvd_path);
}

std::shared_ptr<IVfsNode> VirtualFileSystem::resolve(std::string_view virtual_path) const {
    for (const auto& [mount_pt, host_target] : host_mounts_) {
        if (virtual_path.starts_with(mount_pt)) {
            std::string subpath = std::string(virtual_path.substr(mount_pt.length()));
            if (!subpath.empty() && subpath[0] == '/') {
                subpath = subpath.substr(1);
            }
            std::filesystem::path full_path = host_target / subpath;
            if (std::filesystem::exists(full_path)) {
                return std::make_shared<HostVfsNode>(full_path);
            }
        }
    }
    return nullptr;
}

bool VirtualFileSystem::exists(std::string_view virtual_path) const {
    return resolve(virtual_path) != nullptr;
}

} // namespace papaya::storage
