#pragma once

#include "papaya/common/types.hpp"
#include <string_view>
#include <unordered_map>
#include <optional>

namespace papaya::hle {

// Sony Function NID (64-bit Numeric Identifier hash)
using SonyNid = u64;

struct SonyFunctionMeta {
    std::string_view module_name;
    std::string_view function_name;
    SonyNid nid;
};

// Known Sony NID Constants
namespace Nids {
    // libkernel.prx
    constexpr SonyNid sceKernelAllocateDirectMemory     = 0x6F4E2976A0350B48ULL;
    constexpr SonyNid sceKernelMapDirectMemory          = 0x2A62C66A483E006AULL;
    constexpr SonyNid sceKernelReleaseDirectMemory      = 0x3E54DE42E6E24314ULL;
    constexpr SonyNid sceKernelGetDirectMemorySize      = 0x7E1D8357A684DF21ULL;
    constexpr SonyNid scePthreadCreate                  = 0x9953AE92B5F44AEBULL;
    constexpr SonyNid scePthreadJoin                    = 0x489679230554AE2FULL;
    constexpr SonyNid scePthreadSelf                    = 0x2D7B48443B80ECAEULL;
    constexpr SonyNid scePthreadExit                    = 0x127A37648373BC90ULL;
    constexpr SonyNid scePthreadMutexInit               = 0x7A4C62A860F5F6C6ULL;
    constexpr SonyNid scePthreadMutexLock               = 0x3C49A50162B44C1DULL;
    constexpr SonyNid scePthreadMutexUnlock             = 0x9CE8C697B76D74CEULL;
    constexpr SonyNid scePthreadMutexDestroy            = 0x66A59E77F3227BE8ULL;
    constexpr SonyNid sceKernelCreateEventFlag          = 0x7E8357A8B734431FULL;
    constexpr SonyNid sceKernelSetEventFlag             = 0x82C74801BC384766ULL;
    constexpr SonyNid sceKernelWaitEventFlag            = 0x6E4C62A829384762ULL;
    constexpr SonyNid sceKernelDeleteEventFlag          = 0x2A384762B76D74CEULL;
    constexpr SonyNid sceKernelCreateSema               = 0x5C49A50162B4431DULL;
    constexpr SonyNid sceKernelWaitSema                 = 0x9953AE92B5F4431FULL;
    constexpr SonyNid sceKernelSignalSema               = 0x2A62C66A483E006FULL;
    constexpr SonyNid sceKernelDeleteSema               = 0x3E54DE42E6E24000ULL;
    constexpr SonyNid sceKernelUsleep                   = 0x56784920ABCE1234ULL;
    constexpr SonyNid sceKernelGetProcessTime           = 0x78901234ABCD5678ULL;

    // libSceSysmodule.prx
    constexpr SonyNid sceSysmoduleLoadModule            = 0x87654321ABCDEF01ULL;
    constexpr SonyNid sceSysmoduleUnloadModule          = 0x10FEDCBA12345678ULL;
    constexpr SonyNid sceSysmoduleIsLoaded              = 0x34567890ABCDEF12ULL;

    // libScePad.prx
    constexpr SonyNid scePadInit                        = 0x4B3A82910CFE56A0ULL;
    constexpr SonyNid scePadOpen                        = 0x90283746152435AAULL;
    constexpr SonyNid scePadClose                       = 0x778899AABBCCDDEEULL;
    constexpr SonyNid scePadReadState                   = 0x1122334455667788ULL;
    constexpr SonyNid scePadSetVibration                = 0x9988776655443322ULL;

    // libSceAudioOut.prx
    constexpr SonyNid sceAudioOutInit                   = 0x554433221100FFEEULL;
    constexpr SonyNid sceAudioOutOpen                   = 0xCCDDEEFF00112233ULL;
    constexpr SonyNid sceAudioOutClose                  = 0xAABBCCDDEEFF0011ULL;
    constexpr SonyNid sceAudioOutOutput                 = 0x1234567887654321ULL;
    constexpr SonyNid sceAudioOutSetVolume              = 0x8765432112345678ULL;

    // libSceAgc.prx / libSceGnmDriver.prx
    constexpr SonyNid sceAgcDraw                        = 0xDEADBEEFCAFE0001ULL;
    constexpr SonyNid sceAgcDispatch                    = 0xDEADBEEFCAFE0002ULL;
    constexpr SonyNid sceGnmSubmitCommandBuffers        = 0xDEADBEEFCAFE0003ULL;
    constexpr SonyNid sceGnmFlushGarlic                 = 0xDEADBEEFCAFE0004ULL;

    // libSceFios2.prx
    constexpr SonyNid sceFiosInitialize                 = 0xFEEDFACECAFEBABEULL;
    constexpr SonyNid sceFiosOpSchedule                 = 0xCAFEBABEDEADBEEFULL;
    constexpr SonyNid sceFiosTerminate                  = 0xBADC0FFEEDEADBEEULL;
}

class SonyNidDatabase {
public:
    static std::optional<SonyFunctionMeta> resolve(SonyNid nid);
    static std::optional<SonyNid> lookup(std::string_view module_name, std::string_view function_name);
};

} // namespace papaya::hle
