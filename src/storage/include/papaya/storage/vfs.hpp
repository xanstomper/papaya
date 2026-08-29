#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <unordered_map>

namespace papaya::storage {

enum class FileAccessMode {
    Read,
    Write,
    ReadWrite
};

class IVfsNode {
public:
    virtual ~IVfsNode() = default;
    virtual std::string_view get_name() const = 0;
    virtual bool is_directory() const = 0;
    virtual u64 get_size() const = 0;
    virtual Result<std::vector<u8>> read_all() = 0;
};

class VirtualFileSystem {
public:
    VirtualFileSystem();
    ~VirtualFileSystem();

    Result<> mount(std::string_view mount_point, const std::filesystem::path& host_path);
    Result<> mount_xvd(std::string_view mount_point, const std::filesystem::path& xvd_path);
    
    std::shared_ptr<IVfsNode> resolve(std::string_view virtual_path) const;
    bool exists(std::string_view virtual_path) const;
    Result<std::vector<u8>> read_file(std::string_view virtual_path) const;

private:
    std::unordered_map<std::string, std::filesystem::path> host_mounts_;
};

} // namespace papaya::storage
