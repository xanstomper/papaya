#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/hle/sync_primitives.hpp"
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <array>

namespace papaya::hle {

constexpr u32 TLS_OUT_OF_INDEXES = 0xFFFFFFFF;
constexpr u32 MAX_TLS_SLOTS       = 64;

struct HleThreadInfo {
    u32 tid{0};
    u32 handle{0};
    GuestVirtAddr start_address{0};
    GuestVirtAddr parameter{0};
    GuestVirtAddr stack_base{0};
    u64 stack_size{0};
    bool is_suspended{false};
    bool is_terminated{false};
    u32 exit_code{0};
    std::shared_ptr<HleEvent> completion_event;
};

class ThreadManager {
public:
    explicit ThreadManager(HandleTable& handle_table);
    ~ThreadManager();

    // TLS APIs
    u32 tls_alloc();
    bool tls_free(u32 slot);
    u64 tls_get_value(u32 slot, u32 tid);
    bool tls_set_value(u32 slot, u64 value, u32 tid);

    // Thread APIs
    Result<u32> create_thread(
        GuestVirtAddr start_address,
        GuestVirtAddr parameter,
        u64 stack_size,
        u32 creation_flags
    );

    bool resume_thread(u32 handle);
    bool suspend_thread(u32 handle);
    void exit_thread(u32 exit_code, u32 tid = 0);

    u32 get_current_tid() const;
    void set_current_tid(u32 tid);

    std::shared_ptr<HleThreadInfo> get_thread_by_id(u32 tid) const;
    std::shared_ptr<HleThreadInfo> get_thread_by_handle(u32 handle) const;

private:
    HandleTable& handle_table_;
    mutable std::mutex mutex_;

    // TLS storage: slot -> (tid -> value)
    std::array<bool, MAX_TLS_SLOTS> tls_allocated_{};
    std::array<std::unordered_map<u32, u64>, MAX_TLS_SLOTS> tls_values_{};

    // Thread tracking
    u32 next_tid_{0x1000};
    GuestVirtAddr next_stack_gva_{0x20000000ULL}; // 512MB mark for worker stacks
    std::unordered_map<u32, std::shared_ptr<HleThreadInfo>> threads_by_tid_;
    std::unordered_map<u32, std::shared_ptr<HleThreadInfo>> threads_by_handle_;
};

} // namespace papaya::hle
