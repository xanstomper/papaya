#include "papaya/common/logger.hpp"
#include "papaya/gpu/command_processor.hpp"
#include "papaya/gpu/gcn_registers.hpp"
#include "papaya/gpu/gcn_pm4.hpp"
#include <cassert>
#include <vector>
#include <iostream>

// Helper to encode a PM4 Type 3 packet header
constexpr papaya::u32 make_pm4_type3(papaya::gpu::Pm4Type3OpCode op, papaya::u32 count_minus_1) {
    return (3U << 30) | (static_cast<papaya::u32>(op) << 16) | (count_minus_1 & 0x3FFF);
}

int main() {
    using namespace papaya;
    using namespace papaya::gpu;
    using namespace papaya::gpu::regs;

    log::info("TEST", "Running unit test: test_gcn_state");

    std::vector<u32> cmd_stream;

    // 1. SET_CONTEXT_REG: Configure Render Target 0 (Base, Pitch, Format)
    // Offset 0x00 (CB_COLOR0_BASE), 5 registers: BASE, PITCH, SLICE, VIEW, INFO
    cmd_stream.push_back(make_pm4_type3(Pm4Type3OpCode::SetContextReg, 5));
    cmd_stream.push_back(0x00);                  // reg_start
    cmd_stream.push_back(0x00200000 >> 8);        // CB_COLOR0_BASE (GPA 0x00200000)
    cmd_stream.push_back(1920);                  // CB_COLOR0_PITCH
    cmd_stream.push_back(0);                     // CB_COLOR0_SLICE
    cmd_stream.push_back(0);                     // CB_COLOR0_VIEW
    cmd_stream.push_back(static_cast<u32>(ColorFormat::R8G8B8A8_UNORM) << 2); // CB_COLOR0_INFO

    // 2. SET_CONTEXT_REG: Configure Viewport (1920x1080 centered at (0, 0))
    // Offset 0x10F (PA_CL_VPORT_XSCALE), 4 registers: XSCALE, XOFFSET, YSCALE, YOFFSET
    f32 xscale = 960.0f, xoffset = 960.0f;
    f32 yscale = 540.0f, yoffset = 540.0f;
    cmd_stream.push_back(make_pm4_type3(Pm4Type3OpCode::SetContextReg, 4));
    cmd_stream.push_back(0x10F);
    cmd_stream.push_back(*reinterpret_cast<u32*>(&xscale));
    cmd_stream.push_back(*reinterpret_cast<u32*>(&xoffset));
    cmd_stream.push_back(*reinterpret_cast<u32*>(&yscale));
    cmd_stream.push_back(*reinterpret_cast<u32*>(&yoffset));

    // 3. SET_CONTEXT_REG: Depth Control & Rasterizer
    // Offset 0x203 (DB_DEPTH_CONTROL), 2 registers: DB_DEPTH_CONTROL, PA_SU_SC_MODE_CNTL
    cmd_stream.push_back(make_pm4_type3(Pm4Type3OpCode::SetContextReg, 2));
    cmd_stream.push_back(0x203);
    cmd_stream.push_back(1 | 2 | (static_cast<u32>(CompareOp::Less) << 4)); // Depth Test=1, Write=1, Func=Less
    cmd_stream.push_back(static_cast<u32>(CullMode::Back));                 // Cull Back

    // 4. SET_CONTEXT_REG: Primitive Topology (TriangleList)
    cmd_stream.push_back(make_pm4_type3(Pm4Type3OpCode::SetContextReg, 1));
    cmd_stream.push_back(0x2A1); // VGT_PRIMITIVE_TYPE
    cmd_stream.push_back(static_cast<u32>(PrimitiveTopology::TriangleList));

    // 5. SET_SH_REG: Shader Program addresses & User SGPR constants
    // Offset 0x08 (SPI_SHADER_PGM_LO_VS), 2 registers: LO_VS, HI_VS
    cmd_stream.push_back(make_pm4_type3(Pm4Type3OpCode::SetShReg, 2));
    cmd_stream.push_back(0x08);
    cmd_stream.push_back(0x00405000 >> 8); // VS Program Lo
    cmd_stream.push_back(0);               // VS Program Hi

    // Offset 0x40 (SPI_SHADER_USER_DATA_VS_0)
    cmd_stream.push_back(make_pm4_type3(Pm4Type3OpCode::SetShReg, 1));
    cmd_stream.push_back(0x40);
    cmd_stream.push_back(0xCAFEBABE); // Constant buffer descriptor / pointer

    // 6. DRAW_INDEX_2: Submit indexed draw of 36 vertices (cube)
    // Opcode 0x27: max_size, index_base_lo, index_base_hi, count, initiator (32-bit indices)
    cmd_stream.push_back(make_pm4_type3(Pm4Type3OpCode::DrawIndex2, 5));
    cmd_stream.push_back(0xFFFF);      // max_size
    cmd_stream.push_back(0x00500000);  // index_base_lo
    cmd_stream.push_back(0x00000000);  // index_base_hi
    cmd_stream.push_back(36);          // count (36 indices)
    cmd_stream.push_back(1 << 1);      // initiator (Index32)

    // Parse stream into PM4 packets
    auto packets = Pm4Parser::parse_stream(cmd_stream);
    assert(packets.size() == 7);

    // Process with CommandProcessor
    CommandProcessor cp;
    bool draw_triggered = false;
    DrawCallRecord captured_draw{};

    cp.set_draw_callback([&](const DrawCallRecord& draw) {
        draw_triggered = true;
        captured_draw = draw;
    });

    auto proc_res = cp.process_packets(packets);
    assert(proc_res.has_value());

    // Verify context state
    const auto& st = cp.get_state();
    assert(st.color_targets[0].base_gpa == 0x00200000);
    assert(st.color_targets[0].pitch == 1920);
    assert(st.color_targets[0].format == ColorFormat::R8G8B8A8_UNORM);

    assert(st.viewport.width == 1920.0f);
    assert(st.viewport.height == 1080.0f);
    assert(st.viewport.x == 0.0f);
    assert(st.viewport.y == 0.0f);

    assert(st.depth_target.depth_test_enable == true);
    assert(st.depth_target.depth_write_enable == true);
    assert(st.depth_target.depth_func == CompareOp::Less);
    assert(st.rasterizer.cull_mode == CullMode::Back);
    assert(st.topology == PrimitiveTopology::TriangleList);

    assert(st.vs.program_gpa == 0x00405000);
    assert(st.vs.user_data[0] == 0xCAFEBABE);

    // Verify draw call
    assert(draw_triggered);
    assert(captured_draw.is_indexed);
    assert(captured_draw.count == 36);
    assert(captured_draw.index_base_gpa == 0x00500000);
    assert(captured_draw.index_type == IndexType::Index32);
    assert(captured_draw.topology == PrimitiveTopology::TriangleList);

    assert(cp.get_draw_history().size() == 1);

    log::info("TEST", ">>> test_gcn_state PASSED ALL CHECKS! <<<");
    return 0;
}
