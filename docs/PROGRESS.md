# Papaya — Progress & Hygiene Log

## Strategy (2026-08-31): selectively-native, not "translate everything"

Wine reimplemented ~15-20M LOC of the Win32 surface. Papaya will not win by
out-reimplementing that. The chosen edge (see `docs/game_manifest.md`):

1. **Bridge each game's own runtime** — run Godot/Unity/DotNET/Java in-process
   (or via the host runtime bridge) so the engine does the heavy lifting; papaya
   services only the syscalls that runtime actually issues.
2. **Drive API work from the real corpus, not the alphabet.** The 9 installed
   games import only ~40 unique system DLLs. Manifest-drive: account only those.
3. **Native graphics passthrough** where present (GLX/Mesa via the existing
   GL passthrough), Vulkan as the D3D→Vulkan substrate for the rest.
4. **Data-driven per-title config** (manifest) instead of thousands of LOC.

This is maintenance of progress, not raw API-count.

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

## Tier 2 + Tier 3 (2026-08-31, segmented)

**Tier 2 — newer efficiency layer:**
- *Object mapping (HANDLE->futex/condvar/eventfd):* audited and found ALREADY
  realized — papaya's events/mutexes/sems map to real `pthread`/`std::condition_variable`
  (glibc condvars are futex-backed), so Win32 sync waits block in-kernel, not
  busy-poll. No rework needed; documented as satisfied.
- *ABI-shim codegen:* pure libc-forwardable unregistered surface is ~27 symbols
  (already covered by earlier msvcrt/ucrtbase work), so the marginal upside of a
  signature->libc generator is low; the real generator value was the surface-wide
  stub codegen already shipped.

**Tier 3 — run-the-game's-own-runtime, VERIFIED:**
- `gen_game_manifest.py` -> `docs/game_manifest.md/.json`: 9 installed games, 5
  engine families, only ~40 unique system DLLs imported (proves "translate only
  what the corpus needs").
- `launch_game` Auto-mode now routes by engine: Java JVM bundle -> JVM bridge,
  Godot PCK (separate/standalone) -> host Godot binary, embedded-PCK/exe ->
  NativeWin32 in-process.
- **Verified live:** `TheoTown.jar` launched via papaya -> forked -> exec'd the
  host JVM (`Game execution spawned, PID..., exit 0`). Java game ran via the
  bridge with ZERO Win32 API reimplementation.

**Conclusion:** papaya's efficiency edge is selectively-native execution (bridge
each runtime, translate only what's imported), which Tier 3 now exercises. Next
Tier-3 target: the Godot-Mono/.NET host for SlayTheSpire2.

## Four competitive-parity subsystems (2026-08-31, all four landed)

Verification: ctest 21/21 (was 19), guest suite 21/21, real coverage 883.

1. **D3D->Vulkan present path (`c390224`).** VulkanSwapchain now real: creates a
   VkSwapchainKHR, per-frame vkAcquireNextImageKHR (fence), upload_rgba() maps a
   host-visible staging buffer + records a COPY buffer->image command, and
   presents. Frames now render into the swapchain image (DXVK-style presentation
   foundation; DXBC->SPIR-V shader translation is the remaining big phase).

2. **GDI wide text (`5283b6b`).** CreateFontIndirectW/TextOutW/GetTextMetricsW/
   GetTextExtentPoint32W (ranked gdi32 W-gaps) mapped onto the DC+block-glyph
   rasterizer; strict gdi_text_w guest.

3. **.NET/Mono host bridge groundwork (`06fb06e`).** DotnetBridge discovers +
   dlopens the system libhostfxr and drives init_for_cmdline/run_app/close, so a
   Godot-Mono game can boot via the host CLR instead of papaya reimplementing a
   runtime. VERIFIED loads /usr/lib/dotnet/host/fxr/8.0.30/libhostfxr.so.

4. **CPU translation-backend resolution (`00004a5`).** resolve_backend() picks
   native-x86 when x86-64, else delegates to installed Box64/FEX on ARM64; +
   external-translator detection + a null-pointer UB fix. Box64/FEX are mature
   JITs, so papaya delegates rather than reimplementing a JIT (Phase 12).

Honest remaining: STS2 (Godot-Mono) needs the GodotSharp/.NET-host glue on top
of the DotnetBridge groundwork; D3D drawing (command-list + DXBC->SPIR-V) is
the multi-year shader phase; ARM64 in-papaya JIT remains delegated to Box64/FEX.

## D3D11 shader-translation phase — Stage 1 landed (`4b1d3bc`)

Multi-session phase; modern faster approach: papaya writes the D3D11-state->
Vulkan mapping, NOT a shader compiler. Stage 2 will drive an existing compiler
lib (SPIRV-Cross / DXC / glslang) for DXBC->SPIR-V.

- Stage 1 (done, verified): `dxbc_parse()` — DXBC container parser (header,
  chunk directory, SHDR/SHEX bytecode words, ISGN/OSGN signature elements with
  real 32-byte element layout + semantic names). Bounds-checked; bad magic /
  truncated fail cleanly. Tested with a synthetic-but-valid DXBC blob
  (chunks=2, bytecode, 1 input sig "POSITION"). ctest 22/22.
- Stage 2 (done, verified, replaces the earlier partial decoder): full SM4/SM5
  instruction decoder. The encoding is the REAL on-disk token format, taken
  from wine vkd3d-shader `tpf.c` (wine-mirror/wine @ 9226b10, the field-
  validated parser, byte-exact with native fxc): instruction token = length
  (24-28) | flags (11-13) | opcode (0-7); operand token = reg type (12-19),
  index order (20-21), addressing (22-23/25-26/28-29), extended (31).
  `scripts/gen_sm4_opcodes.py` generates the full 222-opcode table
  (`src/gpu/src/sm4_opcodes.inc`) straight from that reference source; every
  opcode has asm name + dst/src arity. Decoder handles absolute + relative
  indexing, immconst registers, instruction modifiers, DCL payload words, and
  rejects malformed streams. The earlier partial decoder (Stage 2a) used an
  off-by-one opcode table and the wrong bit layout; corrected here.
  Verified: `test_shader_decoder` — MOV/ADD+immconst/dcl_temps/relative
  addressing decode, info lookup, malformed rejection. ctest 23/23.
- Stage 2c (done, verified): `sm4_emit_glsl()` — the FIRST real translation
  output: decoded SM4 instructions -> GLSL body. Supported ALU subset:
  mov/add/mul/mad/min/max/dp2-4, comparisons (eq/ge/lt/ne via bvec-construct),
  exp2/log2/fract/sqrt/inversesqrt/rcp/round_* (banker's rounding approx)/
  dFdx/dFdy, movc (mix), saturate (clamp), immediates with swizzles, write
  masks, src modifiers (-/abs/-abs). dcl_temps/dcl_input/dcl_output become
  vec4 declarations; resource/sampler dcls are consumed but inert in the ALU
  body. Control-flow opcodes are recognized but not emitted (Stage 3). Any
  unsupported opcode makes emission fail cleanly (no partial lies). Verified:
  decode+emit of dcl_input/dcl_output/dcl_temps/mov/mul+immconst/add.sat
  yields exact expected GLSL lines, and an ld instruction is refused.
  ctest 23/23, guest suite 21/21.
- Stage 3 (next): control flow (if/else/loop/switch/break/continue), texture
  loads/samples, constant-buffer indexing, and wiring the emitted GLSL into
  glslang (SPIRV-Cross/DXC) for SPIR-V; then D3D11-state->Vulkan pipeline
  mapping (OMSetRenderTargets->attachments, shaders->pipeline stages).

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
  render; **DXBC/HLSL -> SPIR-V** shader translation has its container parser
  (Stage 1) + SM4/SM5 decoder (Stage 2a); the compiler-lib backend that emits
  SPIR-V is NOT started (needs SPIRV-Cross/DXC/glslang on the host).
- GDI text/font (CreateFontIndirectW, GetTextMetricsW) needs a real font engine
  (FreeType decision not yet made).
- advapi32 services (feature-absent decision not yet made).
- ARM64 x86->ARM64 translator (Box64 parity) is roadmap Phase 12; not started.