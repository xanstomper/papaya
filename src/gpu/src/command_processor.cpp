#include "papaya/gpu/command_processor.hpp"
#include "papaya/gpu/gcn_registers.hpp"
#include "papaya/common/logger.hpp"
#include <cmath>

namespace papaya::gpu {

CommandProcessor::CommandProcessor() = default;
CommandProcessor::~CommandProcessor() = default;

void CommandProcessor::clear_history() {
    draw_history_.clear();
    dispatch_history_.clear();
}

Result<> CommandProcessor::process_packets(std::span<const DecodedPm4Packet> packets) {
    for (const auto& pkt : packets) {
        switch (pkt.opcode) {
            case Pm4Type3OpCode::SetContextReg:
                handle_set_context_reg(pkt);
                break;
            case Pm4Type3OpCode::SetShReg:
                handle_set_sh_reg(pkt);
                break;
            case Pm4Type3OpCode::SetConfigReg:
                handle_set_config_reg(pkt);
                break;
            case Pm4Type3OpCode::DrawIndex2:
                handle_draw_index_2(pkt);
                break;
            case Pm4Type3OpCode::DrawIndexOffset2:
                handle_draw_index_offset_2(pkt);
                break;
            case Pm4Type3OpCode::DispatchDirect:
                handle_dispatch_direct(pkt);
                break;
            case Pm4Type3OpCode::Nop:
            case Pm4Type3OpCode::EventWrite:
            case Pm4Type3OpCode::SurfaceSync:
            case Pm4Type3OpCode::WaitRegMem:
                break;
            default:
                log::trace("GPU_CP", "Opcode 0x{:02X} unhandled in CommandProcessor", static_cast<u8>(pkt.opcode));
                break;
        }
    }
    return {};
}

void CommandProcessor::handle_set_context_reg(const DecodedPm4Packet& pkt) {
    if (pkt.payload.size() < 2) return;

    u32 reg_start = pkt.payload[0]; // Offset relative to 0xA000
    for (size_t i = 1; i < pkt.payload.size(); ++i) {
        u32 reg = reg_start + static_cast<u32>(i - 1);
        u32 val = pkt.payload[i];

        switch (reg) {
            case 0x00: // CB_COLOR0_BASE
                state_.color_targets[0].base_gpa = static_cast<u64>(val) << 8;
                break;
            case 0x01: // CB_COLOR0_PITCH
                state_.color_targets[0].pitch = val & 0xFFFF;
                state_.color_targets[0].width = val & 0xFFFF;
                break;
            case 0x04: // CB_COLOR0_INFO
                state_.color_targets[0].format = static_cast<regs::ColorFormat>((val >> 2) & 0x3F);
                break;
            case 0x80: // PA_SC_WINDOW_OFFSET
                break;
            case 0x90: // PA_SC_SCREEN_SCISSOR_TL
                state_.scissor.left = val & 0x7FFF;
                state_.scissor.top = (val >> 16) & 0x7FFF;
                break;
            case 0x91: // PA_SC_SCREEN_SCISSOR_BR
                state_.scissor.right = val & 0x7FFF;
                state_.scissor.bottom = (val >> 16) & 0x7FFF;
                break;
            case 0x10F: // PA_CL_VPORT_XSCALE
                state_.viewport.width = std::fabs(*reinterpret_cast<const f32*>(&val)) * 2.0f;
                break;
            case 0x110: // PA_CL_VPORT_XOFFSET
                state_.viewport.x = *reinterpret_cast<const f32*>(&val) - (state_.viewport.width / 2.0f);
                break;
            case 0x111: // PA_CL_VPORT_YSCALE
                state_.viewport.height = std::fabs(*reinterpret_cast<const f32*>(&val)) * 2.0f;
                break;
            case 0x112: // PA_CL_VPORT_YOFFSET
                state_.viewport.y = *reinterpret_cast<const f32*>(&val) - (state_.viewport.height / 2.0f);
                break;
            case 0x203: // DB_DEPTH_CONTROL
                state_.depth_target.depth_test_enable = (val & 1) != 0;
                state_.depth_target.depth_write_enable = (val & 2) != 0;
                state_.depth_target.depth_func = static_cast<regs::CompareOp>((val >> 4) & 0x7);
                break;
            case 0x204: // PA_SU_SC_MODE_CNTL
                state_.rasterizer.cull_mode = static_cast<regs::CullMode>(val & 3);
                state_.rasterizer.front_face = ((val >> 2) & 1) ? regs::FrontFace::Clockwise : regs::FrontFace::CounterClockwise;
                state_.rasterizer.wireframe = ((val >> 3) & 1) != 0;
                break;
            case 0x2A1: // VGT_PRIMITIVE_TYPE
                state_.topology = static_cast<regs::PrimitiveTopology>(val & 0x3F);
                break;
            default:
                break;
        }
    }
}

void CommandProcessor::handle_set_sh_reg(const DecodedPm4Packet& pkt) {
    if (pkt.payload.size() < 2) return;

    u32 reg_start = pkt.payload[0]; // Offset relative to 0x2C00
    for (size_t i = 1; i < pkt.payload.size(); ++i) {
        u32 reg = reg_start + static_cast<u32>(i - 1);
        u32 val = pkt.payload[i];

        if (reg == 0x08) { // SPI_SHADER_PGM_LO_VS
            state_.vs.program_gpa = (state_.vs.program_gpa & 0xFFFFFFFF00000000ULL) | (static_cast<u64>(val) << 8);
        } else if (reg == 0x09) { // SPI_SHADER_PGM_HI_VS
            state_.vs.program_gpa = (state_.vs.program_gpa & 0x00000000FFFFFFFFULL) | (static_cast<u64>(val) << 32);
        } else if (reg == 0x0C) { // SPI_SHADER_PGM_LO_PS
            state_.ps.program_gpa = (state_.ps.program_gpa & 0xFFFFFFFF00000000ULL) | (static_cast<u64>(val) << 8);
        } else if (reg == 0x0D) { // SPI_SHADER_PGM_HI_PS
            state_.ps.program_gpa = (state_.ps.program_gpa & 0x00000000FFFFFFFFULL) | (static_cast<u64>(val) << 32);
        } else if (reg == 0x14) { // SPI_SHADER_PGM_LO_CS
            state_.cs.program_gpa = (state_.cs.program_gpa & 0xFFFFFFFF00000000ULL) | (static_cast<u64>(val) << 8);
        } else if (reg == 0x15) { // SPI_SHADER_PGM_HI_CS
            state_.cs.program_gpa = (state_.cs.program_gpa & 0x00000000FFFFFFFFULL) | (static_cast<u64>(val) << 32);
        } else if (reg >= 0x40 && reg <= 0x4F) { // SPI_SHADER_USER_DATA_VS
            state_.vs.user_data[reg - 0x40] = val;
        } else if (reg >= 0x60 && reg <= 0x6F) { // SPI_SHADER_USER_DATA_PS
            state_.ps.user_data[reg - 0x60] = val;
        } else if (reg >= 0x80 && reg <= 0x8F) { // SPI_SHADER_USER_DATA_CS
            state_.cs.user_data[reg - 0x80] = val;
        }
    }
}

void CommandProcessor::handle_set_config_reg(const DecodedPm4Packet&) {
    // Config registers
}

void CommandProcessor::handle_draw_index_2(const DecodedPm4Packet& pkt) {
    if (pkt.payload.size() < 5) return;

    u32 index_base_lo = pkt.payload[1];
    u32 index_base_hi = pkt.payload[2];
    u32 count = pkt.payload[3];
    u32 initiator = pkt.payload[4];

    DrawCallRecord draw{
        .is_indexed = true,
        .count = count,
        .first_index = 0,
        .vertex_offset = 0,
        .instance_count = 1,
        .index_base_gpa = (static_cast<u64>(index_base_hi) << 32) | index_base_lo,
        .index_type = ((initiator >> 1) & 1) ? regs::IndexType::Index32 : regs::IndexType::Index16,
        .topology = state_.topology,
        .pipeline_state = state_
    };

    draw_history_.push_back(draw);
    log::debug("GPU_CP", "Executed DrawIndex2: count={}, index_gpa=0x{:X}, topology={}",
               count, draw.index_base_gpa, static_cast<int>(draw.topology));

    if (draw_cb_) {
        draw_cb_(draw);
    }
}

void CommandProcessor::handle_draw_index_offset_2(const DecodedPm4Packet& pkt) {
    if (pkt.payload.size() < 3) return;

    u32 count = pkt.payload[0];
    u32 offset = pkt.payload[1];
    u32 initiator = pkt.payload[2];

    DrawCallRecord draw{
        .is_indexed = true,
        .count = count,
        .first_index = offset,
        .vertex_offset = 0,
        .instance_count = 1,
        .index_base_gpa = state_.index_buffer.base_gpa,
        .index_type = ((initiator >> 1) & 1) ? regs::IndexType::Index32 : regs::IndexType::Index16,
        .topology = state_.topology,
        .pipeline_state = state_
    };

    draw_history_.push_back(draw);
    log::debug("GPU_CP", "Executed DrawIndexOffset2: count={}, first_index={}", count, offset);

    if (draw_cb_) {
        draw_cb_(draw);
    }
}

void CommandProcessor::handle_dispatch_direct(const DecodedPm4Packet& pkt) {
    if (pkt.payload.size() < 3) return;

    DispatchCallRecord dispatch{
        .group_count_x = pkt.payload[0],
        .group_count_y = pkt.payload[1],
        .group_count_z = pkt.payload[2],
        .cs_program_gpa = state_.cs.program_gpa,
        .cs_user_data = state_.cs.user_data
    };

    dispatch_history_.push_back(dispatch);
    log::debug("GPU_CP", "Executed DispatchDirect: ({}, {}, {}), CS_GPA=0x{:X}",
               dispatch.group_count_x, dispatch.group_count_y, dispatch.group_count_z, dispatch.cs_program_gpa);

    if (dispatch_cb_) {
        dispatch_cb_(dispatch);
    }
}

} // namespace papaya::gpu
