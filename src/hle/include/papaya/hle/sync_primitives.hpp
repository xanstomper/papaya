#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <unordered_map>
#include <memory>
#include <atomic>

namespace papaya::hle {

constexpr u32 WAIT_OBJECT_0    = 0x00000000;
constexpr u32 WAIT_ABANDONED_0 = 0x00000080;
constexpr u32 WAIT_TIMEOUT     = 0x00000102;
constexpr u32 WAIT_FAILED      = 0xFFFFFFFF;
constexpr u32 INFINITE_TIMEOUT = 0xFFFFFFFF;

enum class HandleType : u8 {
    Unknown = 0,
    Event,
    Mutex,
    Semaphore,
    Thread,
    Process,
    File
};

class ISyncObject {
public:
    virtual ~ISyncObject() = default;
    virtual HandleType get_type() const = 0;
    virtual bool wait(u32 timeout_ms = INFINITE_TIMEOUT) = 0;
    virtual void signal() = 0;
};

// Win32 Event Object
class HleEvent : public ISyncObject {
public:
    HleEvent(bool manual_reset, bool initial_state);

    HandleType get_type() const override { return HandleType::Event; }
    bool wait(u32 timeout_ms = INFINITE_TIMEOUT) override;
    void signal() override;

    void set();
    void reset();
    bool is_signaled() const;

private:
    bool manual_reset_{false};
    bool signaled_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

// Win32 Recursive Mutex Object
class HleMutex : public ISyncObject {
public:
    HleMutex(bool initial_owner, u32 owner_tid = 0);

    HandleType get_type() const override { return HandleType::Mutex; }
    bool wait(u32 timeout_ms = INFINITE_TIMEOUT) override;
    void signal() override;

    bool acquire(u32 tid, u32 timeout_ms = INFINITE_TIMEOUT);
    bool release(u32 tid);

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    u32 owner_tid_{0};
    u32 recursion_count_{0};
};

// Win32 Semaphore Object
class HleSemaphore : public ISyncObject {
public:
    HleSemaphore(s32 initial_count, s32 max_count);

    HandleType get_type() const override { return HandleType::Semaphore; }
    bool wait(u32 timeout_ms = INFINITE_TIMEOUT) override;
    void signal() override;

    bool release(s32 release_count, s32* prev_count = nullptr);

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    s32 count_{0};
    s32 max_count_{1};
};

// Linux Futex Wrapper for WaitOnAddress / WakeByAddress
class HleFutex {
public:
    static bool wait_on_address(volatile void* addr, u64 compare_val, size_t size, u32 timeout_ms = INFINITE_TIMEOUT);
    static void wake_by_address_single(const void* addr);
    static void wake_by_address_all(const void* addr);
};

// Thread-safe Generic Handle Table
class HandleTable {
public:
    HandleTable();

    u32 insert(std::shared_ptr<ISyncObject> obj);
    std::shared_ptr<ISyncObject> get(u32 handle) const;
    bool remove(u32 handle);

    u32 wait_for_single_object(u32 handle, u32 timeout_ms);
    u32 wait_for_multiple_objects(std::span<const u32> handles, bool wait_all, u32 timeout_ms);

private:
    u32 next_handle_{0x4}; // Win32 handles start after 0x0
    mutable std::mutex mutex_;
    std::unordered_map<u32, std::shared_ptr<ISyncObject>> handles_;
};

} // namespace papaya::hle
