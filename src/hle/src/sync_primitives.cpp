#include "papaya/hle/sync_primitives.hpp"
#include "papaya/common/logger.hpp"
#include <sys/syscall.h>
#include <linux/futex.h>
#include <unistd.h>
#include <climits>
#include <cstring>

namespace papaya::hle {

// ---------------------------------------------------------------------------
// HleEvent
// ---------------------------------------------------------------------------
HleEvent::HleEvent(bool manual_reset, bool initial_state)
    : manual_reset_(manual_reset), signaled_(initial_state) {}

void HleEvent::set() {
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_ = true;
    if (manual_reset_) {
        cv_.notify_all();
    } else {
        cv_.notify_one();
    }
}

void HleEvent::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_ = false;
}

void HleEvent::signal() {
    set();
}

bool HleEvent::is_signaled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return signaled_;
}

bool HleEvent::wait(u32 timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (signaled_) {
        if (!manual_reset_) {
            signaled_ = false;
        }
        return true;
    }

    if (timeout_ms == 0) {
        return false;
    }

    auto predicate = [this]() { return signaled_; };

    bool success = false;
    if (timeout_ms == INFINITE_TIMEOUT) {
        cv_.wait(lock, predicate);
        success = true;
    } else {
        success = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), predicate);
    }

    if (success && !manual_reset_) {
        signaled_ = false;
    }

    return success;
}

// ---------------------------------------------------------------------------
// HleMutex
// ---------------------------------------------------------------------------
HleMutex::HleMutex(bool initial_owner, u32 owner_tid)
    : owner_tid_(initial_owner ? owner_tid : 0), recursion_count_(initial_owner ? 1 : 0) {}

void HleMutex::signal() {
    release(owner_tid_);
}

bool HleMutex::wait(u32 timeout_ms) {
    return acquire(0, timeout_ms);
}

bool HleMutex::acquire(u32 tid, u32 timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (recursion_count_ == 0 || owner_tid_ == tid) {
        owner_tid_ = tid;
        recursion_count_++;
        return true;
    }

    if (timeout_ms == 0) {
        return false;
    }

    auto predicate = [this, tid]() {
        return recursion_count_ == 0 || owner_tid_ == tid;
    };

    bool success = false;
    if (timeout_ms == INFINITE_TIMEOUT) {
        cv_.wait(lock, predicate);
        success = true;
    } else {
        success = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), predicate);
    }

    if (success) {
        owner_tid_ = tid;
        recursion_count_++;
    }

    return success;
}

bool HleMutex::release(u32 tid) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recursion_count_ == 0 || (owner_tid_ != 0 && tid != 0 && owner_tid_ != tid)) {
        return false;
    }

    recursion_count_--;
    if (recursion_count_ == 0) {
        owner_tid_ = 0;
        cv_.notify_one();
    }
    return true;
}

// ---------------------------------------------------------------------------
// HleSemaphore
// ---------------------------------------------------------------------------
HleSemaphore::HleSemaphore(s32 initial_count, s32 max_count)
    : count_(initial_count), max_count_(max_count) {}

void HleSemaphore::signal() {
    release(1);
}

bool HleSemaphore::wait(u32 timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (count_ > 0) {
        count_--;
        return true;
    }

    if (timeout_ms == 0) {
        return false;
    }

    auto predicate = [this]() { return count_ > 0; };

    bool success = false;
    if (timeout_ms == INFINITE_TIMEOUT) {
        cv_.wait(lock, predicate);
        success = true;
    } else {
        success = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), predicate);
    }

    if (success) {
        count_--;
    }

    return success;
}

bool HleSemaphore::release(s32 release_count, s32* prev_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (prev_count) {
        *prev_count = count_;
    }

    if (count_ + release_count > max_count_) {
        return false;
    }

    count_ += release_count;
    for (s32 i = 0; i < release_count; ++i) {
        cv_.notify_one();
    }
    return true;
}

// ---------------------------------------------------------------------------
// HleFutex
// ---------------------------------------------------------------------------
bool HleFutex::wait_on_address(volatile void* addr, u64 compare_val, size_t size, u32 timeout_ms) {
    if (!addr) return false;

    // Check if value still matches before sleeping
    if (size == 1) {
        if (*reinterpret_cast<volatile u8*>(addr) != static_cast<u8>(compare_val)) return true;
    } else if (size == 2) {
        if (*reinterpret_cast<volatile u16*>(addr) != static_cast<u16>(compare_val)) return true;
    } else if (size == 4) {
        if (*reinterpret_cast<volatile u32*>(addr) != static_cast<u32>(compare_val)) return true;
    } else if (size == 8) {
        if (*reinterpret_cast<volatile u64*>(addr) != compare_val) return true;
    }

    struct timespec ts{};
    struct timespec* timeout_ptr = nullptr;
    if (timeout_ms != INFINITE_TIMEOUT) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        timeout_ptr = &ts;
    }

    int cur_val = *reinterpret_cast<volatile int*>(addr);
    long res = syscall(SYS_futex, const_cast<void*>(reinterpret_cast<volatile void*>(addr)),
                       FUTEX_WAIT_PRIVATE, cur_val, timeout_ptr, nullptr, 0);

    return (res == 0 || errno == EAGAIN);
}

void HleFutex::wake_by_address_single(const void* addr) {
    if (!addr) return;
    syscall(SYS_futex, const_cast<void*>(addr), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}

void HleFutex::wake_by_address_all(const void* addr) {
    if (!addr) return;
    syscall(SYS_futex, const_cast<void*>(addr), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}

// ---------------------------------------------------------------------------
// HandleTable
// ---------------------------------------------------------------------------
HandleTable::HandleTable() = default;

u32 HandleTable::insert(std::shared_ptr<ISyncObject> obj) {
    std::lock_guard<std::mutex> lock(mutex_);
    u32 handle = next_handle_;
    next_handle_ += 4; // Win32 handles are typically multiples of 4
    handles_[handle] = std::move(obj);
    return handle;
}

std::shared_ptr<ISyncObject> HandleTable::get(u32 handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = handles_.find(handle);
    if (it != handles_.end()) {
        return it->second;
    }
    return nullptr;
}

bool HandleTable::remove(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    return handles_.erase(handle) > 0;
}

u32 HandleTable::wait_for_single_object(u32 handle, u32 timeout_ms) {
    auto obj = get(handle);
    if (!obj) {
        return WAIT_FAILED;
    }

    if (obj->wait(timeout_ms)) {
        return WAIT_OBJECT_0;
    } else {
        return WAIT_TIMEOUT;
    }
}

u32 HandleTable::wait_for_multiple_objects(std::span<const u32> handles, bool wait_all, u32 timeout_ms) {
    if (handles.empty() || handles.size() > 64) {
        return WAIT_FAILED;
    }

    if (wait_all) {
        for (u32 h : handles) {
            u32 res = wait_for_single_object(h, timeout_ms);
            if (res != WAIT_OBJECT_0) {
                return res;
            }
        }
        return WAIT_OBJECT_0;
    } else {
        // Wait any
        for (size_t i = 0; i < handles.size(); ++i) {
            u32 res = wait_for_single_object(handles[i], 0);
            if (res == WAIT_OBJECT_0) {
                return WAIT_OBJECT_0 + static_cast<u32>(i);
            }
        }

        // Block on first available
        if (timeout_ms == 0) return WAIT_TIMEOUT;
        auto start = std::chrono::steady_clock::now();

        while (true) {
            for (size_t i = 0; i < handles.size(); ++i) {
                u32 res = wait_for_single_object(handles[i], 1);
                if (res == WAIT_OBJECT_0) {
                    return WAIT_OBJECT_0 + static_cast<u32>(i);
                }
            }

            if (timeout_ms != INFINITE_TIMEOUT) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start
                ).count();
                if (static_cast<u32>(elapsed) >= timeout_ms) {
                    return WAIT_TIMEOUT;
                }
            }
        }
    }
}

} // namespace papaya::hle
