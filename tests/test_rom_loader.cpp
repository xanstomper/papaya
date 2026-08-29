#include "papaya/common/logger.hpp"
#include "papaya/rom/rom_loader.hpp"
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <cstring>

#define TEST_CHECK(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "FAILED: " #expr << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::abort(); \
        } \
    } while (0)

int main() {
    using namespace papaya;
    using namespace papaya::rom;

    log::info("TEST", "Running unit test: test_rom_loader");

    // 1. Generate a mock ISO 9660 disc image (32 sectors of 2048 bytes = 64KB)
    std::filesystem::path mock_iso_path = "./mock_test_game.iso";
    {
        std::vector<u8> iso_data(32 * SECTOR_SIZE_ISO_DATA, 0);
        // Sector 16: Primary Volume Descriptor (offset 0x8000)
        u64 pvd_offset = 16 * SECTOR_SIZE_ISO_DATA;
        iso_data[pvd_offset] = 1; // Type: PVD
        std::memcpy(&iso_data[pvd_offset + 1], "CD001", 5); // Magic
        iso_data[pvd_offset + 6] = 1; // Version

        // Volume ID at offset 40
        const char* vol_id = "PAPAYA_TEST_ISO";
        std::memcpy(&iso_data[pvd_offset + 40], vol_id, std::strlen(vol_id));

        // Application ID (Serial) at offset 592
        const char* app_id = "SLUS-20062";
        std::memcpy(&iso_data[pvd_offset + 592], app_id, std::strlen(app_id));

        // Sector 20: Test data sector
        u64 sec20_offset = 20 * SECTOR_SIZE_ISO_DATA;
        std::memset(&iso_data[sec20_offset], 0x7E, SECTOR_SIZE_ISO_DATA);

        std::ofstream out(mock_iso_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(iso_data.data()), iso_data.size());
    }

    // 2. Test opening file
    RomImageLoader loader;
    auto meta_res = loader.open_file(mock_iso_path);
    TEST_CHECK(meta_res.has_value());
    TEST_CHECK(loader.is_open());

    const auto& meta = loader.get_metadata();
    TEST_CHECK(meta.format == RomFormat::Iso9660);
    TEST_CHECK(meta.title_name == "PAPAYA_TEST_ISO");
    TEST_CHECK(meta.disc_serial_id == "SLUS-20062");
    TEST_CHECK(meta.total_sectors == 32);
    TEST_CHECK(meta.sector_size == SECTOR_SIZE_ISO_DATA);

    // 3. Test reading single sector LBA
    std::vector<u8> sector_buf(SECTOR_SIZE_ISO_DATA, 0);
    TEST_CHECK(loader.read_sector_lba(20, sector_buf).has_value());
    TEST_CHECK(sector_buf[0] == 0x7E);
    TEST_CHECK(sector_buf[SECTOR_SIZE_ISO_DATA - 1] == 0x7E);

    // 4. Test reading span of sectors
    std::vector<u8> span_buf(2 * SECTOR_SIZE_ISO_DATA, 0);
    TEST_CHECK(loader.read_sectors_span(19, 2, span_buf.data()).has_value());
    TEST_CHECK(span_buf[SECTOR_SIZE_ISO_DATA] == 0x7E);

    loader.close();
    TEST_CHECK(!loader.is_open());

    // Clean up temporary mock file
    std::error_code ec;
    std::filesystem::remove(mock_iso_path, ec);

    log::info("TEST", ">>> test_rom_loader PASSED ALL CHECKS! <<<");
    return 0;
}
