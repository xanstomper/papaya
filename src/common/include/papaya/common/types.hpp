#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <memory>
#include <optional>
#include <expected>

namespace papaya {

// Primitive fixed-width aliases
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

using f32 = float;
using f64 = double;

// Common units
constexpr u64 KiB = 1024ULL;
constexpr u64 MiB = 1024ULL * KiB;
constexpr u64 GiB = 1024ULL * MiB;

// Memory Page Sizes
constexpr u64 PAGE_SIZE_4K  = 4096ULL;
constexpr u64 PAGE_SIZE_16K = 16384ULL; // Android 15+ 16KB Page Size
constexpr u64 PAGE_SIZE_2M  = 2ULL * MiB;

// Device Profile Categories
enum class DeviceTier {
    UltraLowEnd,     // Raspberry Pi 5 / BCM2712 / Low-end SBC
    MobileMidTier,   // Snapdragon 7 / MediaTek Dimensity
    MobileHighTier,  // Snapdragon 8 Gen 2 / Gen 3 (AYN Odin 2, Retroid Pocket 5)
    HandheldPC,      // Steam Deck / ASUS ROG Ally / Legion Go
    DesktopLinux     // High-End Linux Desktop (x86-64)
};

// Rendering Performance Optimization Mode
enum class PerformanceMode {
    Native,          // Standard DXVK / No overrides
    Balanced,        // Light LOD bias (+1.0), anisotropic clamp 4x
    Performance,     // Moderate LOD bias (+2.5), 720p internal resolution, post-processing stripped
    PotatoMode       // Aggressive LOD bias (+4.0), 540p internal resolution, 1x1 flat textures, heavy shaders killed
};

// CPU Dynamic Translation Engine
enum class CpuTranslationEngine {
    DirectHostX86,   // Direct native x86-64 execution (Linux PC / Steam Deck)
    Box64Jit,        // Box64 userspace x86/x64 JIT
    FexEmuJit        // FEX-Emu ARM64 translation core
};

// GPU Driver Interceptor Target
enum class GpuDriverProfile {
    MesaTurnip,      // Open-source Adreno Vulkan driver (Qualcomm Snapdragon)
    MesaPanfrost,    // Mali GPU Vulkan driver
    MesaV3DV,        // Raspberry Pi 5 VideoCore VII
    GenericDesktop   // NVIDIA / AMD RADV / Intel ANV
};

} // namespace papaya
