#include "papaya/common/logger.hpp"
#include "papaya/hle/sony_nids.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace papaya;
    using namespace papaya::hle;

    log::info("TEST", "Running unit test: test_sony_nids");

    // 1. Test Resolving known NIDs
    auto direct_mem = SonyNidDatabase::resolve(Nids::sceKernelAllocateDirectMemory);
    assert(direct_mem.has_value());
    assert(direct_mem->module_name == "libkernel.prx");
    assert(direct_mem->function_name == "sceKernelAllocateDirectMemory");

    auto pad_init = SonyNidDatabase::resolve(Nids::scePadInit);
    assert(pad_init.has_value());
    assert(pad_init->module_name == "libScePad.prx");

    auto audio_out = SonyNidDatabase::resolve(Nids::sceAudioOutInit);
    assert(audio_out.has_value());
    assert(audio_out->module_name == "libSceAudioOut.prx");

    auto agc_draw = SonyNidDatabase::resolve(Nids::sceAgcDraw);
    assert(agc_draw.has_value());
    assert(agc_draw->module_name == "libSceAgc.prx");

    // 2. Test Reverse Lookup
    auto lookup_res = SonyNidDatabase::lookup("libkernel.prx", "scePthreadCreate");
    assert(lookup_res.has_value());
    assert(*lookup_res == Nids::scePthreadCreate);

    log::info("TEST", ">>> test_sony_nids PASSED ALL CHECKS! <<<");
    return 0;
}
