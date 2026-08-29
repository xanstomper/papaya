#include "papaya/profile/memory_watchdog.hpp"
#include "papaya/common/logger.hpp"
#include <unistd.h>
#include <fstream>
#include <sstream>

namespace papaya::profile {

MemoryWatchdog::MemoryWatchdog(f32 pressure_threshold)
    : pressure_threshold_(pressure_threshold) {}

void MemoryWatchdog::register_flush_callback(MemoryFlushCallback cb) {
    callbacks_.push_back(std::move(cb));
}

SystemMemoryInfo MemoryWatchdog::query_memory_status() {
    SystemMemoryInfo info{};

    long pages = sysconf(_SC_PHYS_PAGES);
    long av_pages = sysconf(_SC_AVPHYS_PAGES);
    long page_sz = sysconf(_SC_PAGESIZE);

    if (pages > 0 && page_sz > 0) {
        info.total_ram_bytes = static_cast<u64>(pages) * static_cast<u64>(page_sz);
        info.available_ram_bytes = (av_pages > 0) ? (static_cast<u64>(av_pages) * static_cast<u64>(page_sz)) : 0;
        info.used_ram_bytes = (info.total_ram_bytes > info.available_ram_bytes) ? (info.total_ram_bytes - info.available_ram_bytes) : 0;
        info.memory_pressure_ratio = static_cast<f32>(info.used_ram_bytes) / static_cast<f32>(info.total_ram_bytes);
    }

    return info;
}

bool MemoryWatchdog::poll_and_enforce() {
    auto mem = query_memory_status();

    if (mem.memory_pressure_ratio >= pressure_threshold_) {
        is_critical_ = true;
        flush_events_++;
        log::warn("WATCHDOG", "Memory pressure critical ({:.1f}% used, threshold: {:.1f}%)! Flushing texture caches...",
                  mem.memory_pressure_ratio * 100.0f, pressure_threshold_ * 100.0f);

        for (auto& cb : callbacks_) {
            if (cb) cb();
        }
        return true;
    }

    is_critical_ = false;
    return false;
}

} // namespace papaya::profile
