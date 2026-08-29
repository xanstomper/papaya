#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <string>
#include <string_view>
#include <filesystem>

namespace papaya::rom {

class AndroidStorageBridge {
public:
    AndroidStorageBridge() = default;
    ~AndroidStorageBridge() = default;

    // Checks if the URI is an Android Storage Access Framework content URI ("content://...")
    static bool is_content_uri(std::string_view uri);

    // Resolves a URI (either native Linux POSIX path or Android /proc/self/fd/<fd> descriptor)
    static Result<int> open_uri_descriptor(std::string_view uri, u64& out_file_size);

    // Queries total size of an open file descriptor
    static u64 query_descriptor_size(int fd);
};

} // namespace papaya::rom
