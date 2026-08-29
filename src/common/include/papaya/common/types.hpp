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

// PlayStation 4 & PlayStation 5 Hardware Memory Specifications
constexpr u64 PS4_UNIFIED_RAM_SIZE     = 8ULL * GiB;   // 8 GB GDDR5 Unified Space (Orbis)
constexpr u64 PS4_PRO_UNIFIED_RAM_SIZE = 9ULL * GiB;   // 8 GB GDDR5 + 1 GB DDR3
constexpr u64 PS5_UNIFIED_RAM_SIZE     = 16ULL * GiB;  // 16 GB GDDR6 Unified Space (Prospero)
constexpr u64 PS5_PRO_UNIFIED_RAM_SIZE = 18ULL * GiB;  // 16 GB GDDR6 + 2 GB DDR5

// Memory Page Sizes
constexpr u64 PAGE_SIZE_4K             = 4096ULL;
constexpr u64 PAGE_SIZE_2M             = 2ULL * MiB;
constexpr u64 PAGE_SIZE_1G             = 1ULL * GiB;

enum class ConsoleTarget {
    AutoDetect,
    PlayStation4,       // Orbis OS (FreeBSD 9 ABI, AMD GCN)
    PlayStation4Pro,    // Neo
    PlayStation5,       // Prospero OS (FreeBSD 12 ABI, AMD RDNA 2)
    PlayStation5Pro     // Trinity
};

enum class PlatformBackend {
    Kvm,
    Whvp,
    DirectX64,
    Arm64Jit
};

enum class CpuExecutionMode {
    DirectX64,          // Direct Userspace Native Execution on x86-64 Host (mmap PROT_EXEC)
    KvmHypervisor,      // KVM Virtualization Mode
    Arm64DynamicJit     // FEX-Emu / Box64 JIT on ARM64 Host (Snapdragon Android Handhelds)
};

// PlayStation Memory Type Flags (Onion vs Garlic)
enum class SceMemoryType : u32 {
    MainCoherent   = 0x01, // WB_ONION: Fully coherent CPU and GPU cached memory
    MainNonCoherent= 0x02, // WC_GARLIC: High-bandwidth GPU-optimized un-cached memory
    SystemReserved = 0x04,
    DirectPhysical = 0x08
};

} // namespace papaya
