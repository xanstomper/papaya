#pragma once

#include "papaya/common/types.hpp"
#include <vector>
#include <span>

namespace papaya::gpu {

// AMD GCN PM4 Packet Types
enum class Pm4PacketType : u8 {
    Type0 = 0, // Write N consecutive registers
    Type1 = 1, // Write 2 non-consecutive registers
    Type2 = 2, // No-op packet
    Type3 = 3  // Command / compute / draw packet
};

// Common PM4 Type-3 OpCodes
enum class Pm4Type3OpCode : u8 {
    Nop                 = 0x10,
    SetContextReg       = 0x69,
    SetShReg            = 0x76,
    SetConfigReg        = 0x68,
    DrawIndex2          = 0x27,
    DrawIndexOffset2    = 0x35,
    DispatchDirect      = 0x15,
    EventWrite          = 0x46,
    WaitRegMem          = 0x3C,
    SurfaceSync         = 0x43
};

#pragma pack(push, 1)
struct Pm4Header {
    union {
        struct {
            u32 count : 14;           // Bits 0..13
            u32 reserved1 : 2;        // Bits 14..15
            u32 opcode : 8;           // Bits 16..23
            u32 predicate : 1;        // Bit 24
            u32 shader_type : 1;      // Bit 25
            u32 reset_filter_cam : 1; // Bit 26
            u32 auto_index : 1;       // Bit 27
            u32 is_compute : 1;       // Bit 28
            u32 is_binning : 1;       // Bit 29
            u32 type : 2;             // Bits 30..31
        } type3;
        u32 raw;
    };
};
#pragma pack(pop)

struct DecodedPm4Packet {
    Pm4PacketType type;
    Pm4Type3OpCode opcode;
    std::vector<u32> payload;
};

class Pm4Parser {
public:
    static std::vector<DecodedPm4Packet> parse_stream(std::span<const u32> dwords);
};

} // namespace papaya::gpu
