#include "papaya/kernel/ntsync.hpp"
#include "papaya/common/logger.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <unordered_map>
#include <chrono>

namespace papaya::kernel {

// Standard Linux 6.14+ NTSync ioctl structures
#define NTSYNC_IOC_CREATE_SEM    _IOWR('N', 0x80, struct ntsync_sem_args)
#define NTSYNC_IOC_CREATE_MUTEX  _IOWR('N', 0x81, struct ntsync_mutex_args)
#define NTSYNC_IOC_CREATE_EVENT  _IOWR('N', 0x82, struct ntsync_event_args)
#define NTSYNC_IOC_WAIT_ANY      _IOWR('N', 0x83, struct ntsync_wait_args)
#define NTSYNC_IOC_WAIT_ALL      _IOWR('N', 0x84, struct ntsync_wait_args)

struct ntsync_sem_args {
    u32 sem;
    u32 count;
    u32 max;
    u32 flags;
};

struct ntsync_mutex_args {
    u32 mutex;
    u32 owner;
    u32 count;
    u32 flags;
};

struct ntsync_event_args {
    u32 event;
    u32 manual;
    u32 signaled;
    u32 flags;
};

struct ntsync_wait_args {
    u64 timeout;
    u64 objs;
    u32 count;
    u32 index;
    u32 alert;
    u32 pad;
};

enum class NtType { Event, Mutex, Semaphore };

struct FallbackNtObject {
    NtType type;
    bool signaled{false};
    bool manual_reset{false};
    s32 count{0};
    s32 max_count{1};
};

static std::unordered_map<u32, FallbackNtObject> g_fallback_objects;
static std::mutex g_fallback_mutex;
static std::condition_variable g_fallback_cv;
static u32 g_next_handle = 0x100;

NtSyncManager::NtSyncManager() = default;

NtSyncManager::~NtSyncManager() {
    if (ntsync_fd_ >= 0) {
        close(ntsync_fd_);
        ntsync_fd_ = -1;
    }
}

Result<> NtSyncManager::initialize() {
    ntsync_fd_ = open("/dev/ntsync", O_RDWR | O_CLOEXEC);
    if (ntsync_fd_ >= 0) {
        use_kernel_driver_ = true;
        log::info("NTSYNC", "Kernel-Level NTSync driver (/dev/ntsync) successfully engaged! Zero-overhead NT primitives active.");
    } else {
        use_kernel_driver_ = false;
        log::info("NTSYNC", "/dev/ntsync not present on host kernel - engaging ultra-fast userspace futex sync emulation");
    }
    return {};
}

Result<u32> NtSyncManager::create_nt_event(bool manual_reset, bool initial_state) {
    if (use_kernel_driver_ && ntsync_fd_ >= 0) {
        ntsync_event_args args{};
        args.manual = manual_reset ? 1 : 0;
        args.signaled = initial_state ? 1 : 0;
        if (ioctl(ntsync_fd_, NTSYNC_IOC_CREATE_EVENT, &args) == 0) {
            return args.event;
        }
    }

    std::lock_guard<std::mutex> lock(g_fallback_mutex);
    u32 h = g_next_handle++;
    FallbackNtObject obj{};
    obj.type = NtType::Event;
    obj.signaled = initial_state;
    obj.manual_reset = manual_reset;
    g_fallback_objects[h] = obj;
    return h;
}

Result<u32> NtSyncManager::create_nt_mutex(bool initial_owner) {
    if (use_kernel_driver_ && ntsync_fd_ >= 0) {
        ntsync_mutex_args args{};
        args.count = initial_owner ? 1 : 0;
        if (ioctl(ntsync_fd_, NTSYNC_IOC_CREATE_MUTEX, &args) == 0) {
            return args.mutex;
        }
    }

    std::lock_guard<std::mutex> lock(g_fallback_mutex);
    u32 h = g_next_handle++;
    FallbackNtObject obj{};
    obj.type = NtType::Mutex;
    obj.signaled = !initial_owner;
    obj.count = initial_owner ? 1 : 0;
    g_fallback_objects[h] = obj;
    return h;
}

Result<u32> NtSyncManager::create_nt_semaphore(s32 initial_count, s32 max_count) {
    if (use_kernel_driver_ && ntsync_fd_ >= 0) {
        ntsync_sem_args args{};
        args.count = static_cast<u32>(initial_count);
        args.max = static_cast<u32>(max_count);
        if (ioctl(ntsync_fd_, NTSYNC_IOC_CREATE_SEM, &args) == 0) {
            return args.sem;
        }
    }

    std::lock_guard<std::mutex> lock(g_fallback_mutex);
    u32 h = g_next_handle++;
    FallbackNtObject obj{};
    obj.type = NtType::Semaphore;
    obj.signaled = (initial_count > 0);
    obj.count = initial_count;
    obj.max_count = max_count;
    g_fallback_objects[h] = obj;
    return h;
}

bool NtSyncManager::wait_for_nt_object(u32 handle, u32 timeout_ms) {
    if (use_kernel_driver_ && ntsync_fd_ >= 0) {
        ntsync_wait_args args{};
        u32 handles[1] = {handle};
        args.objs = reinterpret_cast<u64>(handles);
        args.count = 1;
        args.timeout = static_cast<u64>(timeout_ms) * 1000000ULL;
        return ioctl(ntsync_fd_, NTSYNC_IOC_WAIT_ANY, &args) == 0;
    }

    std::unique_lock<std::mutex> lock(g_fallback_mutex);
    auto it = g_fallback_objects.find(handle);
    if (it == g_fallback_objects.end()) return false;

    if (it->second.signaled) {
        if (!it->second.manual_reset && it->second.type == NtType::Event) {
            it->second.signaled = false;
        }
        return true;
    }

    auto timeout = std::chrono::milliseconds(timeout_ms);
    return g_fallback_cv.wait_for(lock, timeout, [&]() {
        auto cur = g_fallback_objects.find(handle);
        return cur != g_fallback_objects.end() && cur->second.signaled;
    });
}

bool NtSyncManager::signal_nt_object(u32 handle) {
    std::lock_guard<std::mutex> lock(g_fallback_mutex);
    auto it = g_fallback_objects.find(handle);
    if (it == g_fallback_objects.end()) return false;

    it->second.signaled = true;
    g_fallback_cv.notify_all();
    return true;
}

} // namespace papaya::kernel
