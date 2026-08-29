#include "papaya/common/logger.hpp"
#include "papaya/gpu/gcn_pm4.hpp"
#include <cassert>
#include <vector>

int main() {
    using namespace papaya;
    using namespace papaya::gpu;

    log::info("TEST", "Running unit test: test_gpu_pm4");

    // Construct a synthetic PM4 Type 3 packet stream
    // Type 3 Header:
    // Bits 30..31: type = 3
    // Bits 16..23: opcode = 0x69 (SetContextReg)
    // Bits 0..13:  count = 1 (meaning 2 payload dwords)
    u32 header_val = (3U << 30) | (0x69U << 16) | 1U;
    std::vector<u32> stream = {
        header_val,
        0x0000A000, // Register offset
        0x12345678  // Register value
    };

    auto packets = Pm4Parser::parse_stream(stream);
    assert(packets.size() == 1);
    assert(packets[0].type == Pm4PacketType::Type3);
    assert(packets[0].opcode == Pm4Type3OpCode::SetContextReg);
    assert(packets[0].payload.size() == 2);
    assert(packets[0].payload[0] == 0x0000A000);
    assert(packets[0].payload[1] == 0x12345678);

    log::info("TEST", "test_gpu_pm4 passed!");
    return 0;
}
