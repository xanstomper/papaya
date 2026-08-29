#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/gpu/gcn_pm4.hpp"
#include "papaya/gpu/gcn_state.hpp"
#include <vector>
#include <functional>

namespace papaya::gpu {

struct DrawCallRecord {
    bool is_indexed{false};
    u32 count{0};
    u32 first_index{0};
    u32 vertex_offset{0};
    u32 instance_count{1};
    GuestPhysAddr index_base_gpa{0};
    regs::IndexType index_type{regs::IndexType::Index32};
    regs::PrimitiveTopology topology{regs::PrimitiveTopology::TriangleList};
    GcnContextState pipeline_state{};
};

struct DispatchCallRecord {
    u32 group_count_x{1};
    u32 group_count_y{1};
    u32 group_count_z{1};
    GuestPhysAddr cs_program_gpa{0};
    std::array<u32, 16> cs_user_data{};
};

using DrawCallback = std::function<void(const DrawCallRecord&)>;
using DispatchCallback = std::function<void(const DispatchCallRecord&)>;

class CommandProcessor {
public:
    CommandProcessor();
    ~CommandProcessor();

    Result<> process_packets(std::span<const DecodedPm4Packet> packets);

    void set_draw_callback(DrawCallback cb) { draw_cb_ = std::move(cb); }
    void set_dispatch_callback(DispatchCallback cb) { dispatch_cb_ = std::move(cb); }

    const GcnContextState& get_state() const { return state_; }
    GcnContextState& get_state() { return state_; }

    const std::vector<DrawCallRecord>& get_draw_history() const { return draw_history_; }
    const std::vector<DispatchCallRecord>& get_dispatch_history() const { return dispatch_history_; }

    void clear_history();

private:
    void handle_set_context_reg(const DecodedPm4Packet& pkt);
    void handle_set_sh_reg(const DecodedPm4Packet& pkt);
    void handle_set_config_reg(const DecodedPm4Packet& pkt);
    void handle_draw_index_2(const DecodedPm4Packet& pkt);
    void handle_draw_index_offset_2(const DecodedPm4Packet& pkt);
    void handle_dispatch_direct(const DecodedPm4Packet& pkt);

    GcnContextState state_{};
    DrawCallback draw_cb_;
    DispatchCallback dispatch_cb_;

    std::vector<DrawCallRecord> draw_history_;
    std::vector<DispatchCallRecord> dispatch_history_;
};

} // namespace papaya::gpu
