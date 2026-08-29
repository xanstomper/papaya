#include "papaya/hle/thread_manager.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::hle {

thread_local u32 g_current_tid = 0x1000;

ThreadManager::ThreadManager(HandleTable& handle_table)
    : handle_table_(handle_table) {
    tls_allocated_.fill(false);
}

ThreadManager::~ThreadManager() = default;

u32 ThreadManager::tls_alloc() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (u32 i = 0; i < MAX_TLS_SLOTS; ++i) {
        if (!tls_allocated_[i]) {
            tls_allocated_[i] = true;
            tls_values_[i].clear();
            log::debug("TLS", "Allocated TLS slot index: {}", i);
            return i;
        }
    }
    log::error("TLS", "Out of TLS slots (max {})", MAX_TLS_SLOTS);
    return TLS_OUT_OF_INDEXES;
}

bool ThreadManager::tls_free(u32 slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= MAX_TLS_SLOTS || !tls_allocated_[slot]) {
        return false;
    }
    tls_allocated_[slot] = false;
    tls_values_[slot].clear();
    log::debug("TLS", "Freed TLS slot index: {}", slot);
    return true;
}

u64 ThreadManager::tls_get_value(u32 slot, u32 tid) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= MAX_TLS_SLOTS || !tls_allocated_[slot]) {
        return 0;
    }
    if (tid == 0) tid = g_current_tid;

    auto it = tls_values_[slot].find(tid);
    if (it != tls_values_[slot].end()) {
        return it->second;
    }
    return 0;
}

bool ThreadManager::tls_set_value(u32 slot, u64 value, u32 tid) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= MAX_TLS_SLOTS || !tls_allocated_[slot]) {
        return false;
    }
    if (tid == 0) tid = g_current_tid;

    tls_values_[slot][tid] = value;
    return true;
}

Result<u32> ThreadManager::create_thread(
    GuestVirtAddr start_address,
    GuestVirtAddr parameter,
    u64 stack_size,
    u32 creation_flags
) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (stack_size == 0) {
        stack_size = 1 * MiB; // Default 1MB stack
    }

    u32 tid = next_tid_++;
    GuestVirtAddr stack_base = next_stack_gva_;
    next_stack_gva_ += (stack_size + 0x10000); // 64KB guard page

    auto completion_event = std::make_shared<HleEvent>(true, false); // Manual reset, initially false
    u32 handle = handle_table_.insert(completion_event);

    auto thread_info = std::make_shared<HleThreadInfo>(HleThreadInfo{
        .tid = tid,
        .handle = handle,
        .start_address = start_address,
        .parameter = parameter,
        .stack_base = stack_base,
        .stack_size = stack_size,
        .is_suspended = (creation_flags & 0x00000004) != 0, // CREATE_SUSPENDED
        .is_terminated = false,
        .exit_code = 0,
        .completion_event = completion_event
    });

    threads_by_tid_[tid] = thread_info;
    threads_by_handle_[handle] = thread_info;

    log::info("THREAD", "Created Thread TID: 0x{:X}, Handle: 0x{:X}, Entry: 0x{:X}, Stack: 0x{:X}",
              tid, handle, start_address, stack_base);

    return handle;
}

bool ThreadManager::resume_thread(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = threads_by_handle_.find(handle);
    if (it != threads_by_handle_.end()) {
        it->second->is_suspended = false;
        return true;
    }
    return false;
}

bool ThreadManager::suspend_thread(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = threads_by_handle_.find(handle);
    if (it != threads_by_handle_.end()) {
        it->second->is_suspended = true;
        return true;
    }
    return false;
}

void ThreadManager::exit_thread(u32 exit_code, u32 tid) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tid == 0) tid = g_current_tid;

    auto it = threads_by_tid_.find(tid);
    if (it != threads_by_tid_.end()) {
        it->second->is_terminated = true;
        it->second->exit_code = exit_code;
        if (it->second->completion_event) {
            it->second->completion_event->set();
        }
        log::info("THREAD", "Thread TID 0x{:X} terminated with exit code 0x{:X}", tid, exit_code);
    }
}

u32 ThreadManager::get_current_tid() const {
    return g_current_tid;
}

void ThreadManager::set_current_tid(u32 tid) {
    g_current_tid = tid;
}

std::shared_ptr<HleThreadInfo> ThreadManager::get_thread_by_id(u32 tid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = threads_by_tid_.find(tid);
    return (it != threads_by_tid_.end()) ? it->second : nullptr;
}

std::shared_ptr<HleThreadInfo> ThreadManager::get_thread_by_handle(u32 handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = threads_by_handle_.find(handle);
    return (it != threads_by_handle_.end()) ? it->second : nullptr;
}

} // namespace papaya::hle
