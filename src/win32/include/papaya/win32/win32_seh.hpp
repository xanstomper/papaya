#pragma once

#include "papaya/common/types.hpp"

// x64 Structured Exception Handling (SEH) layer for the native HLE.
//
// The guest runs as native host code (no per-instruction emulation), so we
// cannot unwind mid-stream. Instead we dispatch exceptions at their raise/faulk
// point using the PE exception directory (.pdata/.xdata) and the guest's
// __C_specific_handler scope tables, exactly as Windows RtlDispatchException
// would but driven by host signals / RaiseException calls:
//
//   1. A guest fault (SIGSEGV/SIGFPE) or explicit RaiseException() is caught.
//   2. pdata_find() maps the faulting IP to a RUNTIME_FUNCTION (+ UNWIND_INFO).
//   3. If it's an EHANDLER frame whose handler is __C_specific_handler, the
//      SCOPE_TABLE (at the handler's RVA) is walked to find the scope whose
//      [begin,end) try-range contains the faulting IP.
//   4. The __except filter (or __finally destructor) is invoked with a guest
//      EXCEPTION_RECORD + CONTEXT + DispatcherContext.
//   5. On EXCEPTION_CONTINUE_EXECUTION the handler's recovery address is
//      installed and the caller resumes guest execution there.

namespace papaya::win32 {

// Guest EXCEPTION_RECORD (x64 flattened; enough for filters/handlers).
struct GuestExceptionRecord {
    u32  exception_code{0};
    u32  exception_flags{0};
    void* exception_record{nullptr};   // next
    void* exception_address{nullptr};  // faulting IP
    u32  number_parameters{0};
    u64  exception_information[15]{};
};

// Guest CONTEXT (x64 register snapshot, minimal but handler-complete set).
struct GuestContext {
    u64 rax{}, rcx{}, rdx{}, rbx{}, rsp{}, rbp{}, rsi{}, rdi{};
    u64 r8{}, r9{}, r10{}, r11{}, r12{}, r13{}, r14{}, r15{};
    u64 rip{};
    // Packed the way MSVC's CONTEXT.ExceptionCode sits. Kept minimal.
};

// Guest DispatcherContext (collapsed; holds the .pdata FunctionEntry).
struct GuestDispatcherContext {
    u64 control_pc{0};
    u64 image_base{0};
    void* function_entry{nullptr};     // RVA of RUNTIME_FUNCTION
    void* handler_data{nullptr};       // probably 0 for __C_specific_handler
    void* language_specific_data{nullptr};
    u64  history_table{0};
    u64  scope_index{0};
    u64  est_frame{0};
    u64  context_pointer{0};
    void* handler_base{nullptr};
};

// Registers the loaded image's exception directory + code base for dispatch.
void seh_register_image(void* image_base, void* pdata, u32 pdata_size);

// Finds the RUNTIME_FUNCTION whose [begin_addr,end_addr) RVA range contains the
// given guest IP. Returns the raw header bytes of the UNWIND_INFO (or nullptr).
// Out-params: eh_rva = RVA of the EH handler function if the frame has one.
const void* seh_find_unwind_info(u64 guest_ip, u64 image_base, u64* eh_rva_out);

// Runs __C_specific_handler dispatch for the faulting IP against the scope table
// at handler_base. Returns EXCEPTION_CONTINUE_EXECUTION (0) - meaning the caller
// should resume guest execution at *recovery_ip - or EXCEPTION_CONTINUE_SEARCH (1).
// *recovery_ip is set to the matched __except handler address.
int seh_c_specific_dispatch(const GuestExceptionRecord& rec,
                            GuestContext& ctx,
                            GuestDispatcherContext& disp,
                            u64 image_base,
                            u64* recovery_ip_out);

// Raises a structured exception (used by RaiseException HLE). Returns the final
// disposition and may install a recovery IP. Returns 0 if nobody handled it.
int seh_raise_exception(u32 code, u64 guest_eip_override);

} // namespace papaya::win32