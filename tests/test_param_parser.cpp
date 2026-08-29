#include "papaya/common/logger.hpp"
#include "papaya/storage/param_parser.hpp"
#include <cassert>
#include <vector>
#include <cstring>
#include <iostream>

std::vector<papaya::u8> create_mock_sfo() {
    using namespace papaya;
    using namespace papaya::storage;

    std::vector<u8> buffer(512, 0);
    auto* hdr = reinterpret_cast<SfoHeader*>(buffer.data());
    hdr->magic = SFO_MAGIC;
    hdr->version = 0x0101;
    hdr->key_table_off = sizeof(SfoHeader) + (2 * sizeof(SfoEntry));
    hdr->data_table_off = hdr->key_table_off + 32;
    hdr->entry_count = 2;

    auto* entries = reinterpret_cast<SfoEntry*>(buffer.data() + sizeof(SfoHeader));

    // Entry 0: TITLE_ID = "CUSA12345"
    entries[0].key_offset = 0; // "TITLE_ID"
    entries[0].data_fmt = 0x0204;
    entries[0].data_len = 10;
    entries[0].max_len = 16;
    entries[0].data_offset = 0;

    // Entry 1: TITLE = "Gran Turismo 7"
    entries[1].key_offset = 9; // "TITLE"
    entries[1].data_fmt = 0x0204;
    entries[1].data_len = 15;
    entries[1].max_len = 32;
    entries[1].data_offset = 16;

    // Key table
    char* keys = reinterpret_cast<char*>(buffer.data() + hdr->key_table_off);
    std::memcpy(keys, "TITLE_ID\0TITLE\0", 15);

    // Data table
    u8* data = buffer.data() + hdr->data_table_off;
    std::memcpy(data, "CUSA12345\0", 10);
    std::memcpy(data + 16, "Gran Turismo 7\0", 15);

    return buffer;
}

int main() {
    using namespace papaya;
    using namespace papaya::storage;

    log::info("TEST", "Running unit test: test_param_parser");

    // 1. Test SFO Parser
    auto sfo_data = create_mock_sfo();
    auto sfo_meta = ParamParser::parse_sfo(sfo_data);
    assert(sfo_meta.has_value());
    assert(sfo_meta->title_id == "CUSA12345");
    assert(sfo_meta->app_name == "Gran Turismo 7");
    assert(!sfo_meta->is_ps5);
    log::info("TEST", "SFO metadata parsed: TitleID='{}', Name='{}'", sfo_meta->title_id, sfo_meta->app_name);

    // 2. Test JSON Parser (PS5)
    std::string mock_json = R"({
        "titleId": "PPSA01234",
        "contentId": "UP9000-PPSA01234_00-DEMO000000000001",
        "titleName": "Demon's Souls",
        "version": "01.003.000"
    })";

    auto json_meta = ParamParser::parse_param_json(mock_json);
    assert(json_meta.has_value());
    assert(json_meta->title_id == "PPSA01234");
    assert(json_meta->app_name == "Demon's Souls");
    assert(json_meta->is_ps5);
    log::info("TEST", "PS5 param.json metadata parsed: TitleID='{}', Name='{}'", json_meta->title_id, json_meta->app_name);

    log::info("TEST", ">>> test_param_parser PASSED ALL CHECKS! <<<");
    return 0;
}
