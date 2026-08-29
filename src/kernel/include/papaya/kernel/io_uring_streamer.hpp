#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <filesystem>
#include <vector>
#include <atomic>

namespace papaya::kernel {

struct IoReadRequest {
    int file_descriptor{-1};
    u64 file_offset{0};
    void* destination_buffer{nullptr};
    u64 bytes_to_read{0};
    u64 user_tag{0};
};

class IoUringStreamer {
public:
    explicit IoUringStreamer(u32 queue_depth = 256);
    ~IoUringStreamer();

    Result<> initialize();
    bool is_supported() const { return is_supported_; }

    // Submits batch of asynchronous DirectStorage / ReadFile requests
    Result<> submit_async_read_batch(std::span<const IoReadRequest> requests);

    // Synchronous direct read fallback
    Result<u64> read_sync(int fd, u64 offset, void* dst, u64 bytes);

    u64 get_total_bytes_streamed() const { return total_bytes_streamed_.load(); }
    u64 get_total_io_requests() const { return total_io_requests_.load(); }

private:
    u32 queue_depth_{256};
    bool is_supported_{false};
    std::atomic<u64> total_bytes_streamed_{0};
    std::atomic<u64> total_io_requests_{0};
};

} // namespace papaya::kernel
