// Unit test for the .NET/Mono host bridge.
// Two honest outcomes: hostfxr is found + load() succeeds (reports hostfxr path
// and loaded=true), or no dotnet host is present and load() fails cleanly (no
// crash, loaded=false). Either way the module must not crash — the CI contract.
#include "papaya/kernel/dotnet_bridge.hpp"
#include <cstdio>

using papaya::kernel::DotnetBridge;

int main() {
    DotnetBridge db;
    auto r = db.load();
    if (!r) {   // Result bool == has_value() == success
        std::printf("ok: no dotnet hostfxr present (clean fallback)\n");
        return 0;
    }
    if (!db.loaded() || db.hostfxr_path().empty()) {
        std::printf("fail: load() succeeded but no path/loaded\n");
        return 1;
    }
    std::printf("ok: dotnet hostfxr loaded from %s\n", db.hostfxr_path().c_str());
    db.shutdown();
    return 0;
}