#pragma once

#include "papaya/common/types.hpp"
#include <string>
#include <string_view>
#include <variant>
#include <stdexcept>

namespace papaya {

enum class ErrorCode {
    Success = 0,
    InvalidParameter,
    NotFound,
    FileNotFound,
    OutOfMemory,
    MemoryMappingFailed,
    SteamApiInitFailed,
    SteamAppIdNotFound,
    CpuTranslatorInitFailed,
    PageSizeMismatch16k,
    VulkanInitFailed,
    VulkanDeviceLost,
    PipelineCompilationFailed,
    NtSyncUnavailable,
    IoUringInitFailed,
    AudioInitFailed,
    InputInitFailed,
    UnsupportedOperation
};

inline std::string_view error_to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::Success: return "Success";
        case ErrorCode::InvalidParameter: return "Invalid Parameter";
        case ErrorCode::NotFound: return "Item Not Found";
        case ErrorCode::FileNotFound: return "File Not Found";
        case ErrorCode::OutOfMemory: return "Out of Memory";
        case ErrorCode::MemoryMappingFailed: return "Memory Mapping Failed";
        case ErrorCode::SteamApiInitFailed: return "Steam API Stub Initialization Failed";
        case ErrorCode::SteamAppIdNotFound: return "Steam AppID Not Found (steam_appid.txt missing)";
        case ErrorCode::CpuTranslatorInitFailed: return "CPU Translator (Box64/FEX) Init Failed";
        case ErrorCode::PageSizeMismatch16k: return "16KB Page Size Alignment Constraint";
        case ErrorCode::VulkanInitFailed: return "Vulkan 1.3 Driver Initialization Failed";
        case ErrorCode::VulkanDeviceLost: return "Vulkan Device Lost";
        case ErrorCode::PipelineCompilationFailed: return "Vulkan GPL Pipeline Compilation Failed";
        case ErrorCode::NtSyncUnavailable: return "NTSync Driver (/dev/ntsync) Unavailable";
        case ErrorCode::IoUringInitFailed: return "Linux io_uring Engine Init Failed";
        case ErrorCode::AudioInitFailed: return "Audio Server Connection Failed";
        case ErrorCode::InputInitFailed: return "Virtual XInput Mapper Init Failed";
        case ErrorCode::UnsupportedOperation: return "Unsupported Operation";
        default: return "Unknown Error";
    }
}

template <typename T = void>
class Result;

template <>
class Result<void> {
public:
    Result() : error_(ErrorCode::Success) {}
    Result(ErrorCode err) : error_(err) {}

    bool has_value() const { return error_ == ErrorCode::Success; }
    explicit operator bool() const { return has_value(); }

    ErrorCode error() const { return error_; }

private:
    ErrorCode error_;
};

template <typename T>
class Result {
public:
    Result(const T& val) : storage_(val) {}
    Result(T&& val) : storage_(std::move(val)) {}
    Result(ErrorCode err) : storage_(err) {}

    bool has_value() const { return std::holds_alternative<T>(storage_); }
    explicit operator bool() const { return has_value(); }

    T& value() {
        if (!has_value()) throw std::runtime_error("Result does not contain a value");
        return std::get<T>(storage_);
    }

    const T& value() const {
        if (!has_value()) throw std::runtime_error("Result does not contain a value");
        return std::get<T>(storage_);
    }

    T& operator*() { return value(); }
    const T& operator*() const { return value(); }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    ErrorCode error() const {
        if (has_value()) return ErrorCode::Success;
        return std::get<ErrorCode>(storage_);
    }

private:
    std::variant<T, ErrorCode> storage_;
};

} // namespace papaya
