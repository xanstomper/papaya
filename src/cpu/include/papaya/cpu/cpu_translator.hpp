#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace papaya::cpu {

enum class PageProtection {
    None = 0,
    Read = 1,
    Write = 2,
    Execute = 4,
    ReadWrite = 3,
    ReadExecute = 5,
    ReadWriteExecute = 7
};

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

    Result<void*> allocate_page_aligned(u64 size, PageProtection prot);
    Result<> free_page_aligned(void* ptr, u64 size);
    Result<> protect_page_range(void* ptr, u64 size, PageProtection prot);

    // Sub-page translation tracking
    void register_subpage_mapping(u64 guest_4k_addr, u64 host_aligned_addr, u64 size);

private:
    u64 host_page_size_{PAGE_SIZE_4K};
    std::unordered_map<u64, u64> subpage_table_;
    std::mutex subpage_mutex_;
};

class CpuTranslator {
public:
    explicit CpuTranslator(CpuTranslationEngine engine = CpuTranslationEngine::DirectHostX86);
    ~CpuTranslator();

    Result<> initialize();
    const CpuHostInfo& get_host_info() const { return host_info_; }
    CpuTranslationEngine get_engine() const { return engine_; }
    PageSizeManager& get_page_manager() { return page_manager_; }

    // Resolves the translation backend for the current host: native-x86 when the
    // guest ISA matches the host, else delegates to the best available external
    // translator (Box64/JIT on ARM64). Returns true when a viable backend exists.
    bool resolve_backend();

    // Detects the presence of an external ARM x86->ARM64 translator (Box64/FEX).
    static std::string detect_external_translator();

    // Generates environment variables for JIT execution on ARM (Box64 / FEX)
    std::vector<std::pair<std::string, std::string>> get_environment_overrides() const;

    // Flushes instruction cache for translated JIT blocks
    static void flush_instruction_cache(void* start, void* end);

    // Detects host CPU specs from /proc/cpuinfo or sysconf
    static CpuHostInfo detect_host_cpu();

private:
    CpuTranslationEngine engine_;
    CpuHostInfo host_info_;
    PageSizeManager page_manager_;
    bool is_initialized_{false};
};

} // namespace papaya::cpu
