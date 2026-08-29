#include "papaya/storage/param_parser.hpp"
#include "papaya/common/logger.hpp"
#include <cstring>

namespace papaya::storage {

Result<AppMetadata> ParamParser::parse_sfo(std::span<const u8> sfo_data) {
    if (sfo_data.size() < sizeof(SfoHeader)) {
        return ErrorCode::InvalidParameter;
    }

    const auto* header = reinterpret_cast<const SfoHeader*>(sfo_data.data());
    if (header->magic != SFO_MAGIC) {
        log::error("PARAM", "Invalid SFO magic signature: 0x{:X}", header->magic);
        return ErrorCode::InvalidParameter;
    }

    AppMetadata meta{};
    meta.target = ConsoleTarget::PlayStation4;
    meta.is_ps5 = false;

    u64 entries_sz = header->entry_count * sizeof(SfoEntry);
    if (sizeof(SfoHeader) + entries_sz > sfo_data.size()) {
        return ErrorCode::InvalidParameter;
    }

    const auto* entries = reinterpret_cast<const SfoEntry*>(sfo_data.data() + sizeof(SfoHeader));
    const char* key_table = reinterpret_cast<const char*>(sfo_data.data() + header->key_table_off);
    const u8* data_table = sfo_data.data() + header->data_table_off;

    for (u32 i = 0; i < header->entry_count; ++i) {
        const auto& entry = entries[i];
        if (header->key_table_off + entry.key_offset >= sfo_data.size() ||
            header->data_table_off + entry.data_offset + entry.data_len > sfo_data.size()) {
            continue;
        }

        std::string_view key = key_table + entry.key_offset;
        const u8* val_ptr = data_table + entry.data_offset;

        if (key == "TITLE_ID") {
            meta.title_id = std::string(reinterpret_cast<const char*>(val_ptr), entry.data_len);
            if (!meta.title_id.empty() && meta.title_id.back() == '\0') meta.title_id.pop_back();
        } else if (key == "CONTENT_ID") {
            meta.content_id = std::string(reinterpret_cast<const char*>(val_ptr), entry.data_len);
            if (!meta.content_id.empty() && meta.content_id.back() == '\0') meta.content_id.pop_back();
        } else if (key == "TITLE") {
            meta.app_name = std::string(reinterpret_cast<const char*>(val_ptr), entry.data_len);
            if (!meta.app_name.empty() && meta.app_name.back() == '\0') meta.app_name.pop_back();
        } else if (key == "APP_VER") {
            meta.app_version = std::string(reinterpret_cast<const char*>(val_ptr), entry.data_len);
            if (!meta.app_version.empty() && meta.app_version.back() == '\0') meta.app_version.pop_back();
        } else if (key == "SYSTEM_VER" && entry.data_len >= 4) {
            meta.required_fw_version = *reinterpret_cast<const u32*>(val_ptr);
        }
    }

    log::info("PARAM", "Parsed PS4 param.sfo: TitleID='{}', Name='{}', Version='{}', FW=0x{:08X}",
              meta.title_id, meta.app_name, meta.app_version, meta.required_fw_version);

    return meta;
}

Result<AppMetadata> ParamParser::parse_param_json(std::string_view json_content) {
    AppMetadata meta{};
    meta.target = ConsoleTarget::PlayStation5;
    meta.is_ps5 = true;

    // Fast zero-allocation extraction of core JSON fields
    auto find_json_string = [](std::string_view json, std::string_view key) -> std::string {
        std::string pattern = "\"" + std::string(key) + "\"";
        size_t pos = json.find(pattern);
        if (pos == std::string_view::npos) return "";

        size_t colon = json.find(':', pos + pattern.size());
        if (colon == std::string_view::npos) return "";

        size_t start_quote = json.find('"', colon + 1);
        if (start_quote == std::string_view::npos) return "";

        size_t end_quote = json.find('"', start_quote + 1);
        if (end_quote == std::string_view::npos) return "";

        return std::string(json.substr(start_quote + 1, end_quote - start_quote - 1));
    };

    std::string tid = find_json_string(json_content, "titleId");
    if (!tid.empty()) meta.title_id = tid;

    std::string cid = find_json_string(json_content, "contentId");
    if (!cid.empty()) meta.content_id = cid;

    std::string ver = find_json_string(json_content, "version");
    if (!ver.empty()) meta.app_version = ver;

    std::string name = find_json_string(json_content, "titleName");
    if (!name.empty()) meta.app_name = name;

    log::info("PARAM", "Parsed PS5 param.json: TitleID='{}', Name='{}', Version='{}'",
              meta.title_id, meta.app_name, meta.app_version);

    return meta;
}

} // namespace papaya::storage
