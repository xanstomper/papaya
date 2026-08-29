#include "papaya/rom/android_storage_bridge.hpp"
#include "papaya/common/logger.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace papaya::rom {

bool AndroidStorageBridge::is_content_uri(std::string_view uri) {
    return uri.rfind("content://", 0) == 0;
}

u64 AndroidStorageBridge::query_descriptor_size(int fd) {
    if (fd < 0) return 0;
    struct stat st{};
    if (fstat(fd, &st) == 0) {
        return static_cast<u64>(st.st_size);
    }
    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    return (sz > 0) ? static_cast<u64>(sz) : 0;
}

Result<int> AndroidStorageBridge::open_uri_descriptor(std::string_view uri, u64& out_file_size) {
    out_file_size = 0;

    if (is_content_uri(uri)) {
        log::info("ANDROID_SAF", "Android Content URI detected: '{}'", uri);
        return ErrorCode::AndroidSafOpenFailed;
    }

    int fd = open(std::string(uri).c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return ErrorCode::FileNotFound;
    }

    out_file_size = query_descriptor_size(fd);
    return fd;
}

} // namespace papaya::rom
