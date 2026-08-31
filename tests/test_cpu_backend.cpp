// Unit test for the CPU translation-backend resolution (Tier-4 groundwork).
// On x86-64 host the guest ISA == host ISA -> DirectHostX86, true. On ARM64 it
// resolves Box64/FEX. Either way it must not crash; on this x86 host it must
// resolve native.
#include "papaya/cpu/cpu_translator.hpp"
#include <cstdio>

using papaya::cpu::CpuTranslator;
using papaya::CpuTranslationEngine;

int main() {
    CpuTranslator ct(CpuTranslationEngine::DirectHostX86);
    auto info = ct.get_host_info();
    bool ok = ct.resolve_backend();
    std::printf("host=%s engine=%d backend_ok=%d external=%s\n",
                info.architecture.c_str(), (int)ct.get_engine(), (int)ok,
                CpuTranslator::detect_external_translator().c_str());
    // On x86-64 the backend must be native + viable.
    if (info.is_x86_64) {
        if (!ok || ct.get_engine() != CpuTranslationEngine::DirectHostX86) return 1;
    }
    return 0;
}