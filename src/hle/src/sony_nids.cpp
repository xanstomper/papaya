#include "papaya/hle/sony_nids.hpp"
#include <unordered_map>

namespace papaya::hle {

static const std::unordered_map<SonyNid, SonyFunctionMeta> s_nid_table = {
    { Nids::sceKernelAllocateDirectMemory, { "libkernel.prx", "sceKernelAllocateDirectMemory", Nids::sceKernelAllocateDirectMemory } },
    { Nids::sceKernelMapDirectMemory,      { "libkernel.prx", "sceKernelMapDirectMemory", Nids::sceKernelMapDirectMemory } },
    { Nids::sceKernelReleaseDirectMemory,  { "libkernel.prx", "sceKernelReleaseDirectMemory", Nids::sceKernelReleaseDirectMemory } },
    { Nids::sceKernelGetDirectMemorySize,  { "libkernel.prx", "sceKernelGetDirectMemorySize", Nids::sceKernelGetDirectMemorySize } },
    { Nids::scePthreadCreate,              { "libkernel.prx", "scePthreadCreate", Nids::scePthreadCreate } },
    { Nids::scePthreadJoin,                { "libkernel.prx", "scePthreadJoin", Nids::scePthreadJoin } },
    { Nids::scePthreadSelf,                { "libkernel.prx", "scePthreadSelf", Nids::scePthreadSelf } },
    { Nids::scePthreadExit,                { "libkernel.prx", "scePthreadExit", Nids::scePthreadExit } },
    { Nids::scePthreadMutexInit,           { "libkernel.prx", "scePthreadMutexInit", Nids::scePthreadMutexInit } },
    { Nids::scePthreadMutexLock,           { "libkernel.prx", "scePthreadMutexLock", Nids::scePthreadMutexLock } },
    { Nids::scePthreadMutexUnlock,         { "libkernel.prx", "scePthreadMutexUnlock", Nids::scePthreadMutexUnlock } },
    { Nids::scePthreadMutexDestroy,        { "libkernel.prx", "scePthreadMutexDestroy", Nids::scePthreadMutexDestroy } },
    { Nids::sceKernelCreateEventFlag,      { "libkernel.prx", "sceKernelCreateEventFlag", Nids::sceKernelCreateEventFlag } },
    { Nids::sceKernelSetEventFlag,         { "libkernel.prx", "sceKernelSetEventFlag", Nids::sceKernelSetEventFlag } },
    { Nids::sceKernelWaitEventFlag,        { "libkernel.prx", "sceKernelWaitEventFlag", Nids::sceKernelWaitEventFlag } },
    { Nids::sceKernelDeleteEventFlag,      { "libkernel.prx", "sceKernelDeleteEventFlag", Nids::sceKernelDeleteEventFlag } },
    { Nids::sceKernelCreateSema,           { "libkernel.prx", "sceKernelCreateSema", Nids::sceKernelCreateSema } },
    { Nids::sceKernelWaitSema,             { "libkernel.prx", "sceKernelWaitSema", Nids::sceKernelWaitSema } },
    { Nids::sceKernelSignalSema,           { "libkernel.prx", "sceKernelSignalSema", Nids::sceKernelSignalSema } },
    { Nids::sceKernelDeleteSema,           { "libkernel.prx", "sceKernelDeleteSema", Nids::sceKernelDeleteSema } },
    { Nids::sceKernelUsleep,               { "libkernel.prx", "sceKernelUsleep", Nids::sceKernelUsleep } },
    { Nids::sceKernelGetProcessTime,       { "libkernel.prx", "sceKernelGetProcessTime", Nids::sceKernelGetProcessTime } },

    { Nids::sceSysmoduleLoadModule,        { "libSceSysmodule.prx", "sceSysmoduleLoadModule", Nids::sceSysmoduleLoadModule } },
    { Nids::sceSysmoduleUnloadModule,      { "libSceSysmodule.prx", "sceSysmoduleUnloadModule", Nids::sceSysmoduleUnloadModule } },
    { Nids::sceSysmoduleIsLoaded,          { "libSceSysmodule.prx", "sceSysmoduleIsLoaded", Nids::sceSysmoduleIsLoaded } },

    { Nids::scePadInit,                    { "libScePad.prx", "scePadInit", Nids::scePadInit } },
    { Nids::scePadOpen,                    { "libScePad.prx", "scePadOpen", Nids::scePadOpen } },
    { Nids::scePadClose,                   { "libScePad.prx", "scePadClose", Nids::scePadClose } },
    { Nids::scePadReadState,               { "libScePad.prx", "scePadReadState", Nids::scePadReadState } },
    { Nids::scePadSetVibration,            { "libScePad.prx", "scePadSetVibration", Nids::scePadSetVibration } },

    { Nids::sceAudioOutInit,               { "libSceAudioOut.prx", "sceAudioOutInit", Nids::sceAudioOutInit } },
    { Nids::sceAudioOutOpen,               { "libSceAudioOut.prx", "sceAudioOutOpen", Nids::sceAudioOutOpen } },
    { Nids::sceAudioOutClose,              { "libSceAudioOut.prx", "sceAudioOutClose", Nids::sceAudioOutClose } },
    { Nids::sceAudioOutOutput,             { "libSceAudioOut.prx", "sceAudioOutOutput", Nids::sceAudioOutOutput } },
    { Nids::sceAudioOutSetVolume,          { "libSceAudioOut.prx", "sceAudioOutSetVolume", Nids::sceAudioOutSetVolume } },

    { Nids::sceAgcDraw,                    { "libSceAgc.prx", "sceAgcDraw", Nids::sceAgcDraw } },
    { Nids::sceAgcDispatch,                { "libSceAgc.prx", "sceAgcDispatch", Nids::sceAgcDispatch } },
    { Nids::sceGnmSubmitCommandBuffers,    { "libSceGnmDriver.prx", "sceGnmSubmitCommandBuffers", Nids::sceGnmSubmitCommandBuffers } },
    { Nids::sceGnmFlushGarlic,             { "libSceGnmDriver.prx", "sceGnmFlushGarlic", Nids::sceGnmFlushGarlic } },

    { Nids::sceFiosInitialize,             { "libSceFios2.prx", "sceFiosInitialize", Nids::sceFiosInitialize } },
    { Nids::sceFiosOpSchedule,             { "libSceFios2.prx", "sceFiosOpSchedule", Nids::sceFiosOpSchedule } },
    { Nids::sceFiosTerminate,              { "libSceFios2.prx", "sceFiosTerminate", Nids::sceFiosTerminate } }
};

std::optional<SonyFunctionMeta> SonyNidDatabase::resolve(SonyNid nid) {
    auto it = s_nid_table.find(nid);
    if (it != s_nid_table.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<SonyNid> SonyNidDatabase::lookup(std::string_view module_name, std::string_view function_name) {
    for (const auto& [nid, meta] : s_nid_table) {
        if (meta.module_name == module_name && meta.function_name == function_name) {
            return nid;
        }
    }
    return std::nullopt;
}

} // namespace papaya::hle
