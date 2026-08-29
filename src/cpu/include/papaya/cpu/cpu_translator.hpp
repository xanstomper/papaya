#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace papaya::cpu {

struct CpuHostInfo {
    std::string architecture;
    u64 host_page_size{PAGE_SIZE_4K};
    bool is_arm64{false};
    bool is_x86_64{false};
    bool has_16k_page_size{false};
    u32 cpu_core_count{4};
    std::string cpu_model_name;
};

// 16KB Android 15 Kernel Page Size Compatibility Manager
class PageSizeManager {
public:
    PageSizeManager(u64 host_page_size);
    ~PageSizeManager() = default;

    u64 align_to_host_page(u64 address) const;
    u64 align_size_to_host_page(u64 size) const;
    bool is_4k_emulation_required() const { return host_page_size_ > PAGE_SIZE_4K; }
    u64 get_host_page_size() const { return host_page_size_; }

    Result<void*> allocate_page_aligned(u64 size, int prot);
    Result<> free_page_aligned(void* ptr, u64 size);

private:
    u64 host_page_size_{PAGE_SIZE_4K};
};

class CpuTranslator {
public:
    explicit CpuTranslator(CpuTranslationEngine engine = CpuTranslationEngine::DirectHostX86);
    ~CpuTranslator();

    Result<> initialize();
    const CpuHostInfo& get_host_info() const { return host_info_; }
    CpuTranslationEngine get_engine() const { return engine_; }
    PageSizeManager& get_page_manager() { return page_manager_; }

    // Generates environment variables for JIT execution on ARM (Box64 / FEX)
    std::vector<std::pair<std::string, std::string>> get_environment_overrides() const;

    // Detects host CPU specs from /proc/cpuinfo or sysconf
    static CpuHostInfo detect_host_cpu();

private:
    CpuTranslationEngine engine_;
    CpuHostInfo host_info_;
    PageSizeManager page_manager_;
    bool is_initialized_{false};
};

} // namespace papaya::cpu
