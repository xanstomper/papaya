#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <functional>
#include <vector>
#include <atomic>

namespace papaya::profile {

struct SystemMemoryInfo {
    u64 total_ram_bytes{0};
    u64 available_ram_bytes{0};
    u64 used_ram_bytes{0};
    f32 memory_pressure_ratio{0.0f}; // 0.0 to 1.0
};

using MemoryFlushCallback = std::function<void()>;

class MemoryWatchdog {
public:
    explicit MemoryWatchdog(f32 pressure_threshold = 0.85f);
    ~MemoryWatchdog() = default;

    void register_flush_callback(MemoryFlushCallback cb);
    SystemMemoryInfo query_memory_status();

    bool is_pressure_critical() const { return is_critical_.load(); }
    f32 get_threshold() const { return pressure_threshold_; }

    // Performs check and invokes flush callbacks if above threshold
    bool poll_and_enforce();

    u64 get_flush_event_count() const { return flush_events_.load(); }

private:
    f32 pressure_threshold_{0.85f};
    std::atomic<bool> is_critical_{false};
    std::atomic<u64> flush_events_{0};
    std::vector<MemoryFlushCallback> callbacks_;
};

} // namespace papaya::profile
