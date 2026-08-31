# Papaya — Progress & Hygiene Log

This is the single source of truth for what has actually shipped. It exists so
feature work and health are auditable after any session, instead of being
invisible in scattered commits or a flat coverage percentage.

## Honest metric policy

- The **full-surface percentage (13,397 exports)** is NOT the lead metric: the
  denominator is huge, so it moves slowly (6.2% -> 6.6% this session) even when
  real work lands. It is reported for completeness only.
- The **lead metrics** are:
  1. **Top-1000 real-game APIs implemented** — the APIs the actual game corpus
     (`~/Games`) imports most. This is what compatibility means.
  2. **Guest regression suite** — in-guest executables that prove behavior works
     (not stubs). Currently N passing + ctest M.
  3. **Real HLE function count** and **callable surface**.
  4. **New subsystems** — the GPU/D3D->Vulkan layer, resource loading, etc.

## Session log

### 2026-08-31 session (commits e4ed939..d137f92)

| Area | What shipped | Evidence |
|---|---|---|
| CRT runtime | `_vsnprintf(_s)`, `terminate`, `_c_exit`, `_register_onexit_function`, `_initialize_onexit_table`, `_seh_filter_exe`, `_set_fmode`, `__p___fmode`; api-ms-win-crt-runtime/stdio forwards | guests `vsnprintf_test`, `crt_runtime` |
| kernel32 | `CreateFileMappingA/W`, `MapViewOfFile/Ex`, `UnmapViewOfFile`, `lstrcmpW/i`, `MulDiv`, `IsWow64Process`, `IsBadStringPtr`, `SearchPathW`, `GetDateFormatW/GetTimeFormatW`, console, `VerSetConditionMask` | guest `kernel32_mapfile` |
| ntdll Rtl | `RtlInit/Free[Unicode/Ansi]String`, `RtlUnicode/AnsiStringTo*`, `RtlImageNtHeader`, `RtlGetVersion`, `RtlZeroMemory` | guest `rtl_helpers` |
| user32 | `LoadStringA/W` (real RT_STRING `.rsrc` parsing), `LoadCursor/Icon`, dialog/menu/item helpers | guest `loadstring`, `user32_dialog` |
| comctl32 | `InitCommonControls(Ex)` | guest `crt_runtime` |
| oleaut32 | `VariantInit/Clear`, `SafeArrayCreate/Destroy/AccessData/GetUBound/GetLBound/GetElemsize` | guest `oleaut_variant` |
| **GPU (new subsystem)** | `VulkanSwapchain`: VkInstance + X11 surface + physical device (graphics+present queue) + device + swapchain, acquire/present/upload entry points | test `vulkan_swapchain` (ctest 19), `vulkan_swapchain.cpp` 181 LOC |

**Health:** ctest 19/19, guest suite 20/20, real coverage 879 (6.6%), callable 3051 (22.8%).

### Earlier (this cycle)
- ucrtbase wide-string family + UTF-16LE guest-string correctness fix (`d401ba7`)
- surface-wide codegen stubs (2172) + CRT->libc gets (`1237653`)
- registry delete/create-key (`1239eaf`)

## Hygiene standard (adopted)

1. **Commit atomically per feature** but group tightly-related micro-batches into
   one commit so history shows *systems*, not noise. Prefer one substantive
   commit per keep-going turn (feature + its guest test + doc), not many stubs.
2. **Never delete prior work.** Every feature adds; nothing is removed.
3. **Lead every report with the real metrics** (top-1000 + guest/ctest), not the
   flat full-surface percentage.
4. Every real implementation ships with a **guest test** that proves behavior
   in-guest (exit 0), so "implemented" means "works", not "registered".
5. **Source of truth:** this file must be updated in the same commit as the code.

## Known honest open items (not claimed done)
- D3D->Vulkan: swapchain image acquisition + staging upload so presents actually
  render; **DXBC/HLSL -> SPIR-V** shader translation (the multi-year DXVK-
  equivalent piece) is NOT started in any real form.
- GDI text/font (CreateFontIndirectW, GetTextMetricsW) needs a real font engine
  (FreeType decision not yet made).
- advapi32 services (feature-absent decision not yet made).
- ARM64 x86->ARM64 translator (Box64 parity) is roadmap Phase 12; not started.