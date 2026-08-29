#pragma once

#include "papaya/common/types.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace papaya::rom {

enum class RomFormat {
    Unknown = 0,
    Iso9660,         // Standard DVD/CD ISO Optical Image
    UdfOptical,      // UDF Universal Disk Format (PS2/PS3/Xbox)
    ChdCompressed,   // Compressed Hunks of Data (MAME / CHD v5)
    CsoCompressed,   // Compressed ISO (PSP / PS2)
    BinCue,          // Raw CD-ROM Tracks (2352 byte sectors)
    RvzGameCubeWii,  // RVZ Dolphin GameCube / Wii format
    NspSwitch,       // Nintendo Switch PFS0 Package
    XciSwitch,       // Nintendo Switch Game Card Image
    RawBinary        // Flat executable / cartridge ROM
};

enum class DiscRegion {
    RegionFree = 0,
    NtscNorthAmerica,
    NtscJapan,
    PalEurope
};

constexpr u32 SECTOR_SIZE_ISO_DATA = 2048; // Standard ISO 9660 sector
constexpr u32 SECTOR_SIZE_RAW_CD   = 2352; // Raw CD-ROM Sector (Mode 1 / Mode 2)
constexpr u32 SECTOR_SIZE_CD_SUB   = 2448; // CD-ROM with Subchannel Q/P

struct RomMetadata {
    std::string title_name{"Unknown ROM"};
    std::string disc_serial_id{"PAPAYA-00000"}; // e.g. "SLUS-20062", "CUSA00123", "0100000000010000"
    RomFormat format{RomFormat::Unknown};
    DiscRegion region{DiscRegion::RegionFree};
    u64 total_sectors{0};
    u32 sector_size{SECTOR_SIZE_ISO_DATA};
    u64 total_file_size_bytes{0};
    u32 calculated_crc32{0};
    u32 virtual_steam_appid{480};
    std::string internal_executable_path;
};

} // namespace papaya::rom
