#include "papaya/win32/win32_seh.hpp"
#include "papaya/common/logger.hpp"
#include "papaya/win32/pe_types.hpp"
#include <cstring>
#include <cstdint>

namespace papaya::win32 {

// Registered image exception directory (process-wide; one guest image at a time).
static struct {
    u64      image_base = 0;
    const u8* pdata     = nullptr;
    u32      pdata_size = 0;
    bool     active     = false;
} g_seh;

void seh_register_image(void* image_base, void* pdata, u32 pdata_size) {
    g_seh.image_base = reinterpret_cast<u64>(image_base);
    g_seh.pdata      = static_cast<const u8*>(pdata);
    g_seh.pdata_size = pdata_size;
    g_seh.active     = (pdata != nullptr && pdata_size > 0);
}

// The __C_specific_handler / __CxxFrameHandler entry has a specific structure:
// the RUNTIME_FUNCTION's unwind_info_address points at UNWIND_INFO; if
// EHANDLER (bit 3 of version_flags) is set, the UNWIND_CODE array is followed by
// the handler's RVA (a 32-bit image-relative offset).
static bool read_ehandler_rva(const RuntimeFunction& rf, u64 image_base, u64* eh_rva_out) {
    const u8* uw = reinterpret_cast<const u8*>(image_base + rf.unwind_info_address);
    if (!uw) return false;

    u8 version_flags = uw[0];
    u8 count_of_codes = uw[2];
    bool has_ehandler = (version_flags & 0x08) != 0;   // UNW_FLAG_EHANDLER
    if (!has_ehandler) return false;

    // UNWIND_CODE entries are 2 bytes each. After the count, one 2-byte pad to
    // a dword boundary, then the handler RVA as a u32.
    u32 total_codes_bytes = static_cast<u32>(count_of_codes) * 2;
    u32 hdr_size = 4 + ((total_codes_bytes + 3) & ~3u);  // align dword
    u32 handler_rva = 0;
    std::memcpy(&handler_rva, uw + hdr_size, sizeof(u32));

    // Most x64 EH handlers are __C_specific_handler (msvcrt/ucrt).
    if (eh_rva_out) *eh_rva_out = image_base + handler_rva;
    return true;
}

const void* seh_find_unwind_info(u64 guest_ip, u64 image_base, u64* eh_rva_out) {
    if (!g_seh.active) return nullptr;
    if (eh_rva_out) *eh_rva_out = 0;

    // .pdata is an array of RuntimeFunction (12 bytes each), sorted by begin.
    u64 ip_rva = guest_ip - image_base;
    size_t n = g_seh.pdata_size / sizeof(RuntimeFunction);
    auto* rf = reinterpret_cast<const RuntimeFunction*>(g_seh.pdata);

    // Binary search for the function containing ip_rva.
    size_t lo = 0, hi = n;
    const RuntimeFunction* hit = nullptr;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ip_rva < rf[mid].begin_address)      hi = mid;
        else if (ip_rva >= rf[mid].end_address)  lo = mid + 1;
        else { hit = &rf[mid]; break; }
    }
    if (!hit) return nullptr;

    // RUNTIME_FUNCTION_INDIRECT (bit 0 of unwind_info_address): it points to a
    // RuntimeFunction, not raw unwind info. Follow it (rare, usually for
    // chained/chunked functions).
    u32 uwa = hit->unwind_info_address;
    if (uwa & 1) {
        auto* indirect = reinterpret_cast<const RuntimeFunction*>(image_base + (uwa & ~1u));
        uwa = indirect->unwind_info_address;
    }

    read_ehandler_rva(*hit, image_base, eh_rva_out);
    return reinterpret_cast<const void*>(image_base + (uwa & ~1u));
}

// Resolve a 32-bit "SEH-relative" handler address.
static u64 seh_relative(u32 off, u64 image_base) { return image_base + off; }

int seh_c_specific_dispatch(const GuestExceptionRecord& rec,
                            GuestContext& ctx,
                            GuestDispatcherContext& disp,
                            u64 image_base,
                            u64* recovery_ip_out) {
    if (recovery_ip_out) *recovery_ip_out = 0;
    // disp.handler_data points at the SCOPE_TABLE (set by the unwinder; here the
    // store keeps it alongside the recorded RUNTIME_FUNCTION).
    const ScopeRecord* table = reinterpret_cast<const ScopeRecord*>(disp.handler_data);
    if (!table) return 1;   // CONTINUE_SEARCH

    u64 fault_ip = rec.exception_address ? reinterpret_cast<u64>(rec.exception_address) : disp.control_pc;
    u64 fault_rva = fault_ip - image_base;

    // The scope table is a u32 count followed by that many ScopeRecord entries.
    u32 count = *reinterpret_cast<const u32*>(table);
    const auto* scopes = reinterpret_cast<const ScopeRecord*>(table + 1);
    // Scope begin/end are offsets FROM THE TABLE BASE.
    const u32* table_base = reinterpret_cast<const u32*>(table);

    for (u32 i = 0; i < count; ++i) {
        const auto& sc = scopes[i];
        u32 scope_lo_rva = static_cast<u32>(reinterpret_cast<const u8*>(table_base + sc.begin_address) - reinterpret_cast<const u8*>(image_base));
        u32 scope_hi_rva = static_cast<u32>(reinterpret_cast<const u8*>(table_base + sc.end_address) - reinterpret_cast<const u8*>(image_base));

        if (fault_rva >= scope_lo_rva && fault_rva < scope_hi_rva) {
            // Matched a try block. handler_address is SEH-relative to image base.
            u64 handler_va = seh_relative(sc.handler_address, image_base);
            if (recovery_ip_out) *recovery_ip_out = handler_va;
            return 0;   // CONTINUE_EXECUTION (resume at the handler)
        }
    }
    return 1;   // CONTINUE_SEARCH
}

int seh_raise_exception(u32 code, u64 guest_eip_override) {
    // Explicit RaiseException(): run the same dispatch against the faulting IP.
    GuestExceptionRecord rec{};
    rec.exception_code = code;
    rec.exception_address = reinterpret_cast<void*>(guest_eip_override);
    GuestContext ctx{};
    GuestDispatcherContext disp{};
    disp.control_pc = guest_eip_override;
    disp.image_base = g_seh.image_base;
    // disp.handler_data must be the scope table for the enclosing frame; for an
    // explicit RaiseException the runtime uses the RUNTIME_FUNCTION's handler
    // data, which we conservatively leave null here -> CONTINUE_SEARCH unless a
    // caller wired a specific scope table.
    return 1;
}

} // namespace papaya::win32