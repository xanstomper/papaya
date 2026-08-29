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

// Guest physical and virtual addresses
using GuestPhysAddr = u64;
using GuestVirtAddr = u64;
using HostAddr      = u64;

// Common sizes
constexpr u64 KiB = 1024ULL;
constexpr u64 MiB = 1024ULL * KiB;
constexpr u64 GiB = 1024ULL * MiB;

// Xbox One Hardware Constants
constexpr u64 XBOX_ONE_RAM_SIZE    = 8ULL * GiB;
constexpr u64 XBOX_ONE_ESRAM_SIZE  = 32ULL * MiB;
constexpr u64 XBOX_SERIES_S_RAM    = 10ULL * GiB;
constexpr u64 XBOX_SERIES_X_RAM    = 16ULL * GiB;

// Memory Page Sizes
constexpr u64 PAGE_SIZE_4K         = 4096ULL;
constexpr u64 PAGE_SIZE_2M         = 2ULL * MiB;
constexpr u64 PAGE_SIZE_1G         = 1ULL * GiB;

enum class ConsoleTarget {
    XboxOne,
    XboxOneS,
    XboxOneX,
    XboxSeriesS,
    XboxSeriesX
};

enum class PlatformBackend {
    Kvm,
    Whvp,
    Arm64Jit
};

} // namespace papaya
