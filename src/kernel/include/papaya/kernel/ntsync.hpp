#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <string_view>
#include <mutex>
#include <condition_variable>

namespace papaya::kernel {

class NtSyncManager {
public:
    NtSyncManager();
    ~NtSyncManager();

    Result<> initialize();
    bool is_ntsync_available() const { return ntsync_fd_ >= 0; }

    // Fast kernel NT sync handles (Semaphore, Mutex, Event)
    Result<u32> create_nt_event(bool manual_reset, bool initial_state);
    Result<u32> create_nt_mutex(bool initial_owner);
    Result<u32> create_nt_semaphore(s32 initial_count, s32 max_count);

    bool wait_for_nt_object(u32 handle, u32 timeout_ms);
    bool signal_nt_object(u32 handle);

private:
    int ntsync_fd_{-1};
    bool use_kernel_driver_{false};

    // User-space futex fallback if /dev/ntsync is unavailable
    std::mutex fallback_mutex_;
    std::condition_variable fallback_cv_;
};

} // namespace papaya::kernel
