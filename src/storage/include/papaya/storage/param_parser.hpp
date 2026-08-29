#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <span>

namespace papaya::storage {

constexpr u32 SFO_MAGIC = 0x46535000; // "\0PSF" in little-endian

#pragma pack(push, 1)
struct SfoHeader {
    u32 magic;          // "\0PSF"
    u32 version;        // Format version
    u32 key_table_off;  // Offset to key table
    u32 data_table_off; // Offset to value table
    u32 entry_count;    // Number of entries
};

struct SfoEntry {
    u16 key_offset;
    u16 data_fmt;       // 0x0004: UTF-8, 0x0204: UTF-8 (null-term), 0x0404: u32
    u32 data_len;
    u32 max_len;
    u32 data_offset;
};
#pragma pack(pop)

struct AppMetadata {
    ConsoleTarget target{ConsoleTarget::PlayStation4};
    std::string title_id{"CUSA00000"};
    std::string content_id;
    std::string app_name{"PlayStation Title"};
    std::string app_version{"01.00"};
    u32 required_fw_version{0x05050000};
    bool is_ps5{false};
};

class ParamParser {
public:
    static Result<AppMetadata> parse_sfo(std::span<const u8> sfo_data);
    static Result<AppMetadata> parse_param_json(std::string_view json_content);
};

} // namespace papaya::storage
