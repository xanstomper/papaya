#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <string>
#include <string_view>

namespace papaya::profile {

struct GpuSpoofProfile {
    std::string device_name{"Intel(R) HD Graphics 4000"};
    u32 vendor_id{0x8086};          // Intel
    u32 device_id{0x0166};          // HD 4000
    u64 dedicated_vram_bytes{2ULL * GiB};
    u64 shared_system_ram_bytes{4ULL * GiB};
    u32 driver_version{0x00010000};
};

class HardwareSpoofer {
public:
    explicit HardwareSpoofer(const GpuSpoofProfile& profile = {});
    ~HardwareSpoofer() = default;

    void set_profile(const GpuSpoofProfile& profile) { profile_ = profile; }
    const GpuSpoofProfile& get_profile() const { return profile_; }

    // Pre-configured Spoofing Profiles
    static GpuSpoofProfile get_low_end_fallback_profile();      // Intel HD 4000 (forces lowest game presets)
    static GpuSpoofProfile get_adreno_turnip_profile();         // Qualcomm Adreno 740 / Turnip
    static GpuSpoofProfile get_steam_deck_profile();           // AMD Custom GPU 0405 (VanGogh)
    static GpuSpoofProfile get_geforce_fallback_profile();      // NVIDIA GTX 1050 Mobile

    // Intercepts DXGI Adapter Description queries
    void spoof_dxgi_adapter_desc(
        char* out_description,
        size_t max_desc_len,
        u32& out_vendor_id,
        u32& out_device_id,
        u64& out_dedicated_vram,
        u64& out_shared_ram
    ) const;

    // Intercepts Vulkan Physical Device Properties
    void spoof_vulkan_device_properties(
        char* out_device_name,
        size_t max_name_len,
        u32& out_vendor_id,
        u32& out_device_id,
        u32& out_driver_version
    ) const;

private:
    GpuSpoofProfile profile_;
};

} // namespace papaya::profile
