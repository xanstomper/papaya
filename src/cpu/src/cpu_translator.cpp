#include "papaya/cpu/cpu_translator.hpp"
#include "papaya/common/logger.hpp"
#include <unistd.h>
#include <sys/mman.h>
#include <fstream>
#include <sstream>

namespace papaya::cpu {

static int to_posix_prot(PageProtection prot) {
    int p = PROT_NONE;
    if (static_cast<int>(prot) & static_cast<int>(PageProtection::Read)) p |= PROT_READ;
    if (static_cast<int>(prot) & static_cast<int>(PageProtection::Write)) p |= PROT_WRITE;
    if (static_cast<int>(prot) & static_cast<int>(PageProtection::Execute)) p |= PROT_EXEC;
    return p;
}

PageSizeManager::PageSizeManager(u64 host_page_size)
    : host_page_size_(host_page_size) {}

u64 PageSizeManager::align_to_host_page(u64 address) const {
    return address & ~(host_page_size_ - 1);
}

u64 PageSizeManager::align_size_to_host_page(u64 size) const {
    return (size + host_page_size_ - 1) & ~(host_page_size_ - 1);
}

Result<void*> PageSizeManager::allocate_page_aligned(u64 size, PageProtection prot) {
    u64 aligned_sz = align_size_to_host_page(size);
    int posix_prot = to_posix_prot(prot);
    void* ptr = mmap(nullptr, aligned_sz, posix_prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

Result<> PageSizeManager::protect_page_range(void* ptr, u64 size, PageProtection prot) {
    long os_page_size = sysconf(_SC_PAGESIZE);
    u64 os_mask = (os_page_size > 0 ? static_cast<u64>(os_page_size) : PAGE_SIZE_4K) - 1;
    u64 aligned_addr = reinterpret_cast<u64>(ptr) & ~os_mask;
    u64 offset = reinterpret_cast<u64>(ptr) - aligned_addr;
    u64 aligned_sz = (size + offset + os_mask) & ~os_mask;

    if (mprotect(reinterpret_cast<void*>(aligned_addr), aligned_sz, to_posix_prot(prot)) != 0) {
        return ErrorCode::MemoryMappingFailed;
    }
    return {};
}

void PageSizeManager::register_subpage_mapping(u64 guest_4k_addr, u64 host_aligned_addr, u64 size) {
    std::lock_guard<std::mutex> lock(subpage_mutex_);
    subpage_table_[guest_4k_addr] = host_aligned_addr;
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

// Translate-backend resolution (Tier-4 groundwork). On x86-64 the guest IS the
// host: no translation needed. On ARM64 we delegate to the best installed
// external x86->ARM64 translator (Box64/FEX) rather than reimplement a JIT —
// return whether a viable backend resolves so callers can pick the fallback.
bool CpuTranslator::resolve_backend() {
    if (host_info_.is_x86_64) {
        engine_ = CpuTranslationEngine::DirectHostX86;
        return true;
    }
    // ARM64: delegate to an installed external translator.
    if (!detect_external_translator().empty()) {
        engine_ = CpuTranslationEngine::Box64Jit;
        return true;
    }
    // No external translator on ARM: no viable backend yet (in-papaya JIT is a
    // future phase; roadmap Phase 12).
    engine_ = CpuTranslationEngine::Box64Jit;
    return false;
}

std::string CpuTranslator::detect_external_translator() {
    // Box64 and FEX are the mature x86->ARM64 translators; look for their
    // binaries in the usual locations.
    const char* names[] = {"box64", "box64.box64", "fex", "fex-elf-interpreter", nullptr};
    for (int i = 0; names[i]; ++i) {
        std::string path = std::string("/usr/local/bin/") + names[i];
        if (access(path.c_str(), X_OK) == 0) return names[i];
        path = std::string("/usr/bin/") + names[i];
        if (access(path.c_str(), X_OK) == 0) return names[i];
    }
    return "";
}

void CpuTranslator::flush_instruction_cache(void* start, void* end) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache(reinterpret_cast<char*>(start), reinterpret_cast<char*>(end));
#endif
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
