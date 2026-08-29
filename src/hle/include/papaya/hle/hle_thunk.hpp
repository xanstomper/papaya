#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/hv/vcpu.hpp"
#include "papaya/storage/pe_loader.hpp"
#include <functional>
#include <unordered_map>
#include <string>

namespace papaya::hle {

constexpr u16 HLE_HYPERCALL_IO_PORT = 0x00EB; // I/O Port for HLE guest-to-host dispatch

struct HleCallContext {
    u64 rcx{0};
    u64 rdx{0};
    u64 r8{0};
    u64 r9{0};
    u64 rsp{0};
    u64 rip{0};
    void* host_ram_base{nullptr};
    u64 ram_size{0};

    u64 get_stack_arg(u32 arg_index) const {
        // Win64 ABI: Args 1-4 are in RCX, RDX, R8, R9.
        // Arg 5 is at [RSP + 40], Arg 6 at [RSP + 48], etc.
        if (arg_index < 4) {
            if (arg_index == 0) return rcx;
            if (arg_index == 1) return rdx;
            if (arg_index == 2) return r8;
            if (arg_index == 3) return r9;
        }
        u64 offset = 40 + ((arg_index - 4) * 8);
        u64 gpa = rsp + offset;
        if (gpa + 8 <= ram_size && host_ram_base) {
            const auto* ptr = reinterpret_cast<const u64*>(static_cast<const u8*>(host_ram_base) + gpa);
            return *ptr;
        }
        return 0;
    }
};

using HleFunction = std::function<u64(HleCallContext& ctx)>;

struct HleExportSymbol {
    u32 thunk_id{0};
    std::string module_name;
    std::string function_name;
    GuestPhysAddr trampoline_gpa{0};
    HleFunction handler;
};

class HleThunkManager {
public:
    HleThunkManager(GuestPhysAddr thunk_pool_base_gpa = 0x00100000ULL);

    void register_function(
        std::string_view module_name,
        std::string_view function_name,
        HleFunction handler
    );

    Result<> bind_imports(
        std::span<const storage::PeImportEntry> imports,
        void* host_ram_base,
        u64 ram_size
    );

    Result<u64> execute_thunk(
        u32 thunk_id,
        hv::IVcpu& vcpu,
        void* host_ram_base,
        u64 ram_size
    );

    void write_trampolines_to_guest(void* host_ram_base, u64 ram_size);

    const std::unordered_map<u32, HleExportSymbol>& get_exports() const { return exports_by_id_; }

private:
    GuestPhysAddr thunk_pool_base_gpa_{0x00100000ULL};
    u32 next_thunk_id_{1};
    std::unordered_map<std::string, u32> lookup_by_name_;
    std::unordered_map<u32, HleExportSymbol> exports_by_id_;
};

} // namespace papaya::hle
