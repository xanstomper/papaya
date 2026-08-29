#pragma once

#include "papaya/common/types.hpp"
#include <string>
#include <vector>

namespace papaya::hle {

struct ProcessContext {
    u32 process_id{1};
    std::string title_id;
    std::string process_name;
    GuestPhysAddr entry_point{0};
    GuestPhysAddr base_address{0};
    u64 memory_size{0};
};

} // namespace papaya::hle
