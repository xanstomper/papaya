#include "papaya/profile/hardware_spoofer.hpp"
#include "papaya/common/logger.hpp"
#include <cstring>
#include <algorithm>

namespace papaya::profile {

HardwareSpoofer::HardwareSpoofer(const GpuSpoofProfile& profile)
    : profile_(profile) {}

GpuSpoofProfile HardwareSpoofer::get_low_end_fallback_profile() {
    return {
        .device_name = "Intel(R) HD Graphics 4000",
        .vendor_id = 0x8086,
        .device_id = 0x0166,
        .dedicated_vram_bytes = 1024ULL * MiB,
        .shared_system_ram_bytes = 2048ULL * MiB,
        .driver_version = 0x000A0000
    };
}

GpuSpoofProfile HardwareSpoofer::get_adreno_turnip_profile() {
    return {
        .device_name = "Adreno (TM) 740 (Mesa Turnip)",
        .vendor_id = 0x5143, // Qualcomm
        .device_id = 0x0740,
        .dedicated_vram_bytes = 4ULL * GiB,
        .shared_system_ram_bytes = 8ULL * GiB,
        .driver_version = 0x00180000
    };
}

GpuSpoofProfile HardwareSpoofer::get_steam_deck_profile() {
    return {
        .device_name = "AMD Custom GPU 0405 (RADV VANGOGH)",
        .vendor_id = 0x1002, // AMD
        .device_id = 0x163F,
        .dedicated_vram_bytes = 4ULL * GiB,
        .shared_system_ram_bytes = 12ULL * GiB,
        .driver_version = 0x00170000
    };
}

GpuSpoofProfile HardwareSpoofer::get_geforce_fallback_profile() {
    return {
        .device_name = "NVIDIA GeForce GTX 1050",
        .vendor_id = 0x10DE, // NVIDIA
        .device_id = 0x1C8D,
        .dedicated_vram_bytes = 3ULL * GiB,
        .shared_system_ram_bytes = 4ULL * GiB,
        .driver_version = 0x021A0000
    };
}

void HardwareSpoofer::spoof_dxgi_adapter_desc(
    char* out_description,
    size_t max_desc_len,
    u32& out_vendor_id,
    u32& out_device_id,
    u64& out_dedicated_vram,
    u64& out_shared_ram
) const {
    if (out_description && max_desc_len > 0) {
        std::strncpy(out_description, profile_.device_name.c_str(), max_desc_len - 1);
        out_description[max_desc_len - 1] = '\0';
    }
    out_vendor_id = profile_.vendor_id;
    out_device_id = profile_.device_id;
    out_dedicated_vram = profile_.dedicated_vram_bytes;
    out_shared_ram = profile_.shared_system_ram_bytes;

    log::info("SPOOF", "Spoofed DXGI Adapter Query: '{}' (Vendor: 0x{:X}, VRAM: {} MB)",
              profile_.device_name, out_vendor_id, out_dedicated_vram / MiB);
}

void HardwareSpoofer::spoof_vulkan_device_properties(
    char* out_device_name,
    size_t max_name_len,
    u32& out_vendor_id,
    u32& out_device_id,
    u32& out_driver_version
) const {
    if (out_device_name && max_name_len > 0) {
        std::strncpy(out_device_name, profile_.device_name.c_str(), max_name_len - 1);
        out_device_name[max_name_len - 1] = '\0';
    }
    out_vendor_id = profile_.vendor_id;
    out_device_id = profile_.device_id;
    out_driver_version = profile_.driver_version;
}

} // namespace papaya::profile
