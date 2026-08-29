#pragma once

#include "papaya/common/types.hpp"
#include "papaya/gpu/gcn_registers.hpp"
#include <array>

namespace papaya::gpu {

struct RenderTargetState {
    GuestPhysAddr base_gpa{0};
    u32 pitch{0};
    u32 width{0};
    u32 height{0};
    regs::ColorFormat format{regs::ColorFormat::Invalid};
    u8 write_mask{0x0F}; // RGBA
    bool blend_enable{false};
    u32 blend_control{0};
};

struct DepthStencilState {
    GuestPhysAddr base_gpa{0};
    u32 width{0};
    u32 height{0};
    bool depth_test_enable{false};
    bool depth_write_enable{false};
    regs::CompareOp depth_func{regs::CompareOp::Less};
    bool stencil_test_enable{false};
};

struct ViewportState {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 width{1920.0f};
    f32 height{1080.0f};
    f32 min_depth{0.0f};
    f32 max_depth{1.0f};
};

struct ScissorState {
    u32 left{0};
    u32 top{0};
    u32 right{1920};
    u32 bottom{1080};
};

struct RasterizerState {
    regs::CullMode cull_mode{regs::CullMode::Back};
    regs::FrontFace front_face{regs::FrontFace::CounterClockwise};
    bool wireframe{false};
    f32 depth_bias{0.0f};
};

struct IndexBufferState {
    GuestPhysAddr base_gpa{0};
    u32 count{0};
    regs::IndexType index_type{regs::IndexType::Index32};
    u32 index_offset{0};
};

struct ShaderStageState {
    GuestPhysAddr program_gpa{0};
    u32 vgpr_count{0};
    u32 sgpr_count{0};
    std::array<u32, 16> user_data{};
};

struct GcnContextState {
    std::array<RenderTargetState, 8> color_targets{};
    DepthStencilState depth_target{};
    ViewportState viewport{};
    ScissorState scissor{};
    RasterizerState rasterizer{};
    regs::PrimitiveTopology topology{regs::PrimitiveTopology::TriangleList};

    ShaderStageState vs{};
    ShaderStageState ps{};
    ShaderStageState cs{};

    IndexBufferState index_buffer{};

    void reset();
};

} // namespace papaya::gpu
