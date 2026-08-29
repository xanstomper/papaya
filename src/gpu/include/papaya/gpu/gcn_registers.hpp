#pragma once

#include "papaya/common/types.hpp"

namespace papaya::gpu::regs {

// Register bank offset bases
constexpr u32 CONTEXT_REG_BASE = 0xA000;
constexpr u32 SH_REG_BASE      = 0x2C00;
constexpr u32 CONFIG_REG_BASE  = 0x2000;

// Context Registers (0xA000 - 0xA3FF)
constexpr u32 CB_COLOR0_BASE           = 0xA000;
constexpr u32 CB_COLOR0_PITCH          = 0xA001;
constexpr u32 CB_COLOR0_SLICE          = 0xA002;
constexpr u32 CB_COLOR0_VIEW           = 0xA003;
constexpr u32 CB_COLOR0_INFO           = 0xA004;
constexpr u32 CB_COLOR0_ATTRIB         = 0xA005;

constexpr u32 DB_DEPTH_BASE            = 0xA006;
constexpr u32 DB_DEPTH_INFO            = 0xA00F;
constexpr u32 DB_DEPTH_SIZE            = 0xA007;
constexpr u32 DB_RENDER_CONTROL        = 0xA00C;

constexpr u32 PA_SC_WINDOW_OFFSET      = 0xA080;
constexpr u32 PA_SC_SCREEN_SCISSOR_TL  = 0xA090;
constexpr u32 PA_SC_SCREEN_SCISSOR_BR  = 0xA091;

constexpr u32 CB_TARGET_MASK           = 0xA08E;
constexpr u32 CB_SHADER_MASK           = 0xA08F;

constexpr u32 PA_CL_VPORT_XSCALE       = 0xA10F;
constexpr u32 PA_CL_VPORT_XOFFSET      = 0xA110;
constexpr u32 PA_CL_VPORT_YSCALE       = 0xA111;
constexpr u32 PA_CL_VPORT_YOFFSET      = 0xA112;
constexpr u32 PA_CL_VPORT_ZSCALE       = 0xA113;
constexpr u32 PA_CL_VPORT_ZOFFSET      = 0xA114;

constexpr u32 CB_BLEND0_CONTROL        = 0xA1E0;
constexpr u32 CB_BLEND1_CONTROL        = 0xA1E1;

constexpr u32 PA_SU_SC_MODE_CNTL       = 0xA204;
constexpr u32 DB_DEPTH_CONTROL         = 0xA203;

constexpr u32 VGT_PRIMITIVE_TYPE       = 0xA2A1;
constexpr u32 VGT_INDEX_TYPE           = 0xA2A6;
constexpr u32 VGT_NUM_INDICES          = 0xA2A5;

// Shader / SH Registers (0x2C00 - 0x2FFF)
constexpr u32 SPI_SHADER_PGM_LO_VS     = 0x2C08;
constexpr u32 SPI_SHADER_PGM_HI_VS     = 0x2C09;
constexpr u32 SPI_SHADER_PGM_RSRC1_VS  = 0x2C0A;
constexpr u32 SPI_SHADER_PGM_RSRC2_VS  = 0x2C0B;

constexpr u32 SPI_SHADER_USER_DATA_VS_0 = 0x2C40;
constexpr u32 SPI_SHADER_USER_DATA_VS_15 = 0x2C4F;

constexpr u32 SPI_SHADER_PGM_LO_PS     = 0x2C0C;
constexpr u32 SPI_SHADER_PGM_HI_PS     = 0x2C0D;
constexpr u32 SPI_SHADER_PGM_RSRC1_PS  = 0x2C0E;
constexpr u32 SPI_SHADER_PGM_RSRC2_PS  = 0x2C0F;

constexpr u32 SPI_SHADER_USER_DATA_PS_0 = 0x2C60;
constexpr u32 SPI_SHADER_USER_DATA_PS_15 = 0x2C6F;

constexpr u32 SPI_SHADER_PGM_LO_CS     = 0x2C14;
constexpr u32 SPI_SHADER_PGM_HI_CS     = 0x2C15;

constexpr u32 SPI_SHADER_USER_DATA_CS_0 = 0x2C80;
constexpr u32 SPI_SHADER_USER_DATA_CS_15 = 0x2C8F;

// Common GCN Formats
enum class ColorFormat : u8 {
    Invalid = 0,
    R8_UNORM = 1,
    R8G8_UNORM = 2,
    R8G8B8A8_UNORM = 8,
    B8G8R8A8_UNORM = 10,
    R10G10B10A2_UNORM = 13,
    R16G16B16A16_FLOAT = 22,
    R32G32B32A32_FLOAT = 26
};

enum class PrimitiveTopology : u8 {
    PointList = 0,
    LineList = 1,
    LineStrip = 2,
    TriangleList = 3,
    TriangleFan = 4,
    TriangleStrip = 5,
    Patch = 6
};

enum class IndexType : u8 {
    Index16 = 0,
    Index32 = 1
};

enum class CullMode : u8 {
    None = 0,
    Front = 1,
    Back = 2,
    FrontAndBack = 3
};

enum class FrontFace : u8 {
    CounterClockwise = 0,
    Clockwise = 1
};

enum class CompareOp : u8 {
    Never = 0,
    Less = 1,
    Equal = 2,
    LessOrEqual = 3,
    Greater = 4,
    NotEqual = 5,
    GreaterOrEqual = 6,
    Always = 7
};

} // namespace papaya::gpu::regs
