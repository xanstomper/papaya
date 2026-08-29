#include "papaya/cpu/cpu_translator.hpp"
#include "papaya/common/logger.hpp"
#include <unistd.h>
#include <sys/mman.h>
#include <fstream>
#include <sstream>

namespace papaya::cpu {

PageSizeManager::PageSizeManager(u64 host_page_size)
    : host_page_size_(host_page_size) {}

u64 PageSizeManager::align_to_host_page(u64 address) const {
    return address & ~(host_page_size_ - 1);
}

u64 PageSizeManager::align_size_to_host_page(u64 size) const {
    return (size + host_page_size_ - 1) & ~(host_page_size_ - 1);
}

Result<void*> PageSizeManager::allocate_page_aligned(u64 size, int prot) {
    u64 aligned_sz = align_size_to_host_page(size);
    void* ptr = mmap(nullptr, aligned_sz, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return ErrorCode::MemoryMappingFailed;
    }
    return ptr;
}

Result<> PageSizeManager::free_page_aligned(void* ptr, u64 size) {
    u64 aligned_sz = align_size_to_host_page(size);
    if (munmap(ptr, aligned_sz) != 0) {
        return ErrorCode::MemoryMappingFailed;
    }
    return {};
}

CpuTranslator::CpuTranslator(CpuTranslationEngine engine)
    : engine_(engine),
      host_info_(detect_host_cpu()),
      page_manager_(host_info_.host_page_size) {}

CpuTranslator::~CpuTranslator() = default;

CpuHostInfo CpuTranslator::detect_host_cpu() {
    CpuHostInfo info{};

    long ps = sysconf(_SC_PAGESIZE);
    info.host_page_size = (ps > 0) ? static_cast<u64>(ps) : PAGE_SIZE_4K;
    info.has_16k_page_size = (info.host_page_size >= PAGE_SIZE_16K);

    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    info.cpu_core_count = (cores > 0) ? static_cast<u32>(cores) : 4;

#if defined(__aarch64__) || defined(_M_ARM64)
    info.architecture = "aarch64";
    info.is_arm64 = true;
    info.is_x86_64 = false;
#elif defined(__x86_64__) || defined(_M_X64)
    info.architecture = "x86_64";
    info.is_arm64 = false;
    info.is_x86_64 = true;
#else
    info.architecture = "unknown";
#endif

    // Read CPU model from /proc/cpuinfo
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.find("model name") != std::string::npos || line.find("Hardware") != std::string::npos) {
            auto colon = line.find(':');
            if (colon != std::string::npos && colon + 2 < line.size()) {
                info.cpu_model_name = line.substr(colon + 2);
                break;
            }
        }
    }
    if (info.cpu_model_name.empty()) {
        info.cpu_model_name = info.architecture + " generic processor";
    }

    return info;
}

Result<> CpuTranslator::initialize() {
    log::info("CPU", "Initializing CPU Execution Core [Arch: {}, Model: '{}', PageSize: {} KB, 16K Mode: {}]",
              host_info_.architecture, host_info_.cpu_model_name,
              host_info_.host_page_size / KiB, host_info_.has_16k_page_size ? "YES" : "NO");

    if (host_info_.is_arm64 && engine_ == CpuTranslationEngine::DirectHostX86) {
        log::warn("CPU", "Running on ARM64 host with DirectHostX86 engine - switching to Box64/FEX JIT mode");
        engine_ = CpuTranslationEngine::Box64Jit;
    }

    if (host_info_.has_16k_page_size) {
        log::info("CPU", "Android 15+ 16KB Page Size active: Sub-page translation layer engaged for 4KB guest compatibility");
    }

    is_initialized_ = true;
    return {};
}

std::vector<std::pair<std::string, std::string>> CpuTranslator::get_environment_overrides() const {
    std::vector<std::pair<std::string, std::string>> envs;

    if (engine_ == CpuTranslationEngine::Box64Jit) {
        envs.emplace_back("BOX64_DYNAREC", "1");
        envs.emplace_back("BOX64_DYNAREC_FASTNAN", "1");
        envs.emplace_back("BOX64_DYNAREC_SAFEFLAGS", "1");
        envs.emplace_back("BOX64_DYNAREC_BIGBLOCK", "1");
        envs.emplace_back("BOX64_DYNAREC_STRONGMEM", "1");
    } else if (engine_ == CpuTranslationEngine::FexEmuJit) {
        envs.emplace_back("FEX_TSO_ENABLED", "1");
        envs.emplace_back("FEX_SMC_MODE", "full");
    }

    return envs;
}

} // namespace papaya::cpu
