#include "papaya/gpu/gcn_pm4.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::gpu {

std::vector<DecodedPm4Packet> Pm4Parser::parse_stream(std::span<const u32> dwords) {
    std::vector<DecodedPm4Packet> packets;
    size_t i = 0;

    while (i < dwords.size()) {
        Pm4Header header{.raw = dwords[i++]};
        u32 packet_type = header.type3.type;

        if (packet_type == static_cast<u32>(Pm4PacketType::Type3)) {
            u32 count = header.type3.count + 1; // PM4 count is N - 1
            auto opcode = static_cast<Pm4Type3OpCode>(header.type3.opcode);

            DecodedPm4Packet pkt;
            pkt.type = Pm4PacketType::Type3;
            pkt.opcode = opcode;

            for (u32 c = 0; c < count && i < dwords.size(); ++c) {
                pkt.payload.push_back(dwords[i++]);
            }

            packets.push_back(std::move(pkt));
        } else if (packet_type == static_cast<u32>(Pm4PacketType::Type2)) {
            // NOP Packet
            continue;
        } else {
            // Type 0 or Type 1
            i++;
        }
    }

    return packets;
}

} // namespace papaya::gpu
