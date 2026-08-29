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
    HypervisorInitFailed,
    KvmUnavailable,
    VcpuCreationFailed,
    VcpuRunFailed,
    MemoryMappingFailed,
    XvdInvalidSignature,
    XvdHeaderCorrupt,
    XvdKeyNotFound,
    ElfInvalidMagic,
    ElfUnsupportedClass,
    ElfCorruptHeaders,
    PrxRelocationFailed,
    SceModuleNotFound,
    SyscallNotImplemented,
    GpuInitFailed,
    VulkanDeviceLost,
    AudioInitFailed,
    UnsupportedOperation
};

inline std::string_view error_to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::Success: return "Success";
        case ErrorCode::InvalidParameter: return "Invalid Parameter";
        case ErrorCode::NotFound: return "Item Not Found";
        case ErrorCode::FileNotFound: return "File Not Found";
        case ErrorCode::OutOfMemory: return "Out of Memory";
        case ErrorCode::HypervisorInitFailed: return "Hypervisor Initialization Failed";
        case ErrorCode::KvmUnavailable: return "KVM Device Unavailable (/dev/kvm)";
        case ErrorCode::VcpuCreationFailed: return "vCPU Creation Failed";
        case ErrorCode::VcpuRunFailed: return "vCPU Run Loop Failed";
        case ErrorCode::MemoryMappingFailed: return "Guest Memory Mapping Failed";
        case ErrorCode::XvdInvalidSignature: return "XVD Container Invalid Signature";
        case ErrorCode::XvdHeaderCorrupt: return "XVD Header Corrupt";
        case ErrorCode::XvdKeyNotFound: return "XVD Decryption Key Not Found";
        case ErrorCode::ElfInvalidMagic: return "ELF Binary Invalid Magic Signature";
        case ErrorCode::ElfUnsupportedClass: return "ELF Binary Unsupported Class (Requires 64-bit AMD64)";
        case ErrorCode::ElfCorruptHeaders: return "ELF Headers Corrupted";
        case ErrorCode::PrxRelocationFailed: return "PRX Dynamic Relocation Failed";
        case ErrorCode::SceModuleNotFound: return "Sony System Module (.prx) Not Found";
        case ErrorCode::SyscallNotImplemented: return "FreeBSD / Sony Syscall Not Implemented";
        case ErrorCode::GpuInitFailed: return "GPU Core Initialization Failed";
        case ErrorCode::VulkanDeviceLost: return "Vulkan Device Lost";
        case ErrorCode::AudioInitFailed: return "Audio Subsystem Initialization Failed";
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
