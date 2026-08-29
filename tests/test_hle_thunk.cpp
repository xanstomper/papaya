#include "papaya/common/logger.hpp"
#include "papaya/hle/hle_thunk.hpp"
#include "papaya/storage/pe_loader.hpp"
#include <cassert>
#include <vector>
#include <iostream>

int main() {
    using namespace papaya;
    using namespace papaya::hle;

    log::info("TEST", "Running unit test: test_hle_thunk");

    std::vector<u8> guest_ram(16 * 1024 * 1024, 0);

    HleThunkManager thunk_mgr(0x00100000);

    // Register a test HLE API
    bool api_invoked = false;
    thunk_mgr.register_function("kernel32.dll", "AddNumbers", [&](HleCallContext& ctx) -> u64 {
        api_invoked = true;
        return ctx.rcx + ctx.rdx; // Return sum of 1st and 2nd arguments
    });

    // Write trampolines to memory
    thunk_mgr.write_trampolines_to_guest(guest_ram.data(), guest_ram.size());

    // Verify trampoline bytes at 0x00100000:
    // mov eax, 1 (thunk_id 1); out 0xEB, al; ret
    const u8* tramp = guest_ram.data() + 0x00100000;
    assert(tramp[0] == 0xB8); // mov eax, imm32
    assert(tramp[1] == 1);
    assert(tramp[5] == 0xE6); // out imm8, al
    assert(tramp[6] == 0xEB); // port 0xEB
    assert(tramp[7] == 0xC3); // ret

    // Test import binding
    std::vector<storage::PeImportEntry> imports = {
        {
            .module_name = "kernel32.dll",
            .function_name = "AddNumbers",
            .ordinal = 0,
            .is_ordinal = false,
            .iat_gva = 0x00402000 // IAT entry address in guest memory
        }
    };

    auto bind_res = thunk_mgr.bind_imports(imports, guest_ram.data(), guest_ram.size());
    assert(bind_res.has_value());

    // Verify IAT entry was patched with trampoline address (0x00100000)
    const auto* iat_entry = reinterpret_cast<const u64*>(guest_ram.data() + 0x00402000);
    assert(*iat_entry == 0x00100000);

    // Test direct call context invocation
    HleCallContext ctx{
        .rcx = 100,
        .rdx = 250,
        .r8 = 0,
        .r9 = 0,
        .rsp = 0x8000,
        .rip = 0x00100000,
        .host_ram_base = guest_ram.data(),
        .ram_size = guest_ram.size()
    };

    const auto& exports = thunk_mgr.get_exports();
    assert(exports.size() >= 1);
    auto it = exports.find(1);
    assert(it != exports.end());

    u64 result = it->second.handler(ctx);
    assert(api_invoked);
    assert(result == 350);

    log::info("TEST", "test_hle_thunk passed! (Result: 100 + 250 = {})", result);
    return 0;
}
