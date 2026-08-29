#include "papaya/hle/hle_thunk.hpp"
#include "papaya/common/logger.hpp"
#include <algorithm>
#include <cstring>

namespace papaya::hle {

static std::string make_symbol_key(std::string_view mod, std::string_view fn) {
    std::string key;
    key.reserve(mod.size() + 1 + fn.size());
    for (char c : mod) key.push_back(static_cast<char>(std::tolower(c)));
    key.push_back('!');
    for (char c : fn) key.push_back(static_cast<char>(std::tolower(c)));
    return key;
}

HleThunkManager::HleThunkManager(GuestPhysAddr thunk_pool_base_gpa)
    : thunk_pool_base_gpa_(thunk_pool_base_gpa) {}

void HleThunkManager::register_function(
    std::string_view module_name,
    std::string_view function_name,
    HleFunction handler
) {
    std::string key = make_symbol_key(module_name, function_name);
    u32 id = next_thunk_id_++;

    // Each trampoline is 16 bytes aligned
    GuestPhysAddr gpa = thunk_pool_base_gpa_ + ((id - 1) * 16);

    HleExportSymbol sym{
        .thunk_id = id,
        .module_name = std::string(module_name),
        .function_name = std::string(function_name),
        .trampoline_gpa = gpa,
        .handler = std::move(handler)
    };

    lookup_by_name_[key] = id;
    exports_by_id_[id] = std::move(sym);

    log::debug("HLE", "Registered export #{}: '{}!{}' at Trampoline GPA 0x{:X}",
               id, module_name, function_name, gpa);
}

void HleThunkManager::write_trampolines_to_guest(void* host_ram_base, u64 ram_size) {
    if (!host_ram_base) return;

    for (const auto& [id, sym] : exports_by_id_) {
        if (sym.trampoline_gpa + 16 > ram_size) continue;

        auto* ptr = static_cast<u8*>(host_ram_base) + sym.trampoline_gpa;

        // Trampoline machine code (16 bytes):
        // 0xB8, id0, id1, id2, id3  (mov eax, id)
        // 0xE6, 0xEB                (out 0xEB, al)
        // 0xC3                      (ret)
        // 0x90, 0x90, ...           (nop padding)
        ptr[0] = 0xB8;
        ptr[1] = static_cast<u8>(id & 0xFF);
        ptr[2] = static_cast<u8>((id >> 8) & 0xFF);
        ptr[3] = static_cast<u8>((id >> 16) & 0xFF);
        ptr[4] = static_cast<u8>((id >> 24) & 0xFF);
        ptr[5] = 0xE6;
        ptr[6] = static_cast<u8>(HLE_HYPERCALL_IO_PORT);
        ptr[7] = 0xC3;
        for (int i = 8; i < 16; ++i) ptr[i] = 0x90;
    }

    log::info("HLE", "Written {} HLE trampolines to guest memory pool at 0x{:X}",
              exports_by_id_.size(), thunk_pool_base_gpa_);
}

Result<> HleThunkManager::bind_imports(
    std::span<const storage::PeImportEntry> imports,
    void* host_ram_base,
    u64 ram_size
) {
    if (!host_ram_base) {
        return ErrorCode::InvalidParameter;
    }

    u32 resolved_count = 0;
    u32 stubbed_count = 0;

    for (const auto& imp : imports) {
        std::string key = make_symbol_key(imp.module_name, imp.function_name);
        auto it = lookup_by_name_.find(key);

        GuestPhysAddr trampoline_gpa = 0;
        if (it != lookup_by_name_.end()) {
            trampoline_gpa = exports_by_id_[it->second].trampoline_gpa;
            resolved_count++;
        } else {
            // Dynamically register a default stub for unimplemented import
            register_function(imp.module_name, imp.function_name, [name = imp.function_name, mod = imp.module_name](HleCallContext& ctx) -> u64 {
                log::warn("HLE", "Unimplemented import called: '{}!{}' (RCX=0x{:X}, RDX=0x{:X})",
                          mod, name, ctx.rcx, ctx.rdx);
                return 0; // Return generic NULL/0
            });
            std::string new_key = make_symbol_key(imp.module_name, imp.function_name);
            u32 new_id = lookup_by_name_[new_key];
            trampoline_gpa = exports_by_id_[new_id].trampoline_gpa;
            stubbed_count++;
        }

        // Patch IAT in guest memory
        if (imp.iat_gva + sizeof(u64) <= ram_size) {
            auto* iat_ptr = reinterpret_cast<u64*>(static_cast<u8*>(host_ram_base) + imp.iat_gva);
            *iat_ptr = trampoline_gpa;
            log::trace("HLE", "Patched IAT [0x{:X}] -> 0x{:X} ('{}!{}')",
                       imp.iat_gva, trampoline_gpa, imp.module_name, imp.function_name);
        }
    }

    // Refresh memory trampolines with any newly created stubs
    write_trampolines_to_guest(host_ram_base, ram_size);

    log::info("HLE", "Bound {} PE imports ({} resolved, {} stubbed)",
              imports.size(), resolved_count, stubbed_count);
    return {};
}

Result<u64> HleThunkManager::execute_thunk(
    u32 thunk_id,
    hv::IVcpu& vcpu,
    void* host_ram_base,
    u64 ram_size
) {
    auto it = exports_by_id_.find(thunk_id);
    if (it == exports_by_id_.end()) {
        log::error("HLE", "Invalid Thunk ID: {}", thunk_id);
        return ErrorCode::InvalidParameter;
    }

    const auto& sym = it->second;

    auto regs_res = vcpu.get_registers();
    if (!regs_res) {
        return regs_res.error();
    }
    auto regs = *regs_res;

    HleCallContext ctx{
        .rcx = regs.rcx,
        .rdx = regs.rdx,
        .r8  = regs.r8,
        .r9  = regs.r9,
        .rsp = regs.rsp,
        .rip = regs.rip,
        .host_ram_base = host_ram_base,
        .ram_size = ram_size
    };

    log::debug("HLE", "Calling '{}!{}' [ID #{}] from RIP 0x{:X}",
               sym.module_name, sym.function_name, thunk_id, regs.rip);

    u64 ret_val = sym.handler ? sym.handler(ctx) : 0;

    // Set return value in RAX
    regs.rax = ret_val;

    auto set_res = vcpu.set_registers(regs);
    if (!set_res) {
        return set_res.error();
    }

    return ret_val;
}

} // namespace papaya::hle
