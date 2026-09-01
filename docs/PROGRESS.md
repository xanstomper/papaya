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
- Stage 2d (done, verified): control-flow emission. `sm4_emit_glsl()` now emits
  if/else/endif (any(cond != vec4(0.0))), for(;;) loops, break/continue (+
  conditioned breakc/continuec), discard, retc, with proper indentation. SM4
  declarations that don't change the ALU body (dcl_globalFlags, cbuffer,
  sampler, resource, indexable_temp, topology/primitive dcls) are consumed as
  inert. switch/case is REFUSED (not silently dropped): the all-vec4-f32
  representation cannot express an integer switch faithfully. cb-register
  reads are refused until constant-buffer support lands (Stage 3), instead of
  emitting undefined identifiers. Verified: decode+emit of if/else/loop/
  breakc/continue/discard yields the expected GLSL, and switch fails cleanly.
  ctest 23/23, guest suite 21/21.
- Stage 3a (done, verified): constant-buffer reads. `dcl_constantbuffer cbN[M]`
  emits `uniform vec4 cbN[M];` (operand idx0 = buffer, idx1 = size, from
  vkd3d's exact layout); instruction cb operands (encoded order 2: idx0 =
  buffer, idx1 = element) emit `cbN[i]` reads with swizzle support. Also
  fixed a latent GLSL correctness bug: full-vec4 RHS assigned to a partial
  write mask is invalid GLSL, so identity-swizzled sources are now sliced to
  the dst mask (`r1.xy = (r0.xy * ...)`) matching SM4 semantics exactly.
  Decoder captures opcode-token bits 11-19 into `DecodedInstruction::aux`
  (interpolation mode/global flags/resource type/sample count) for the
  texture stage. Relative cb addressing still refused (honest). Verified:
  decode+emit of dcl_cb + mov/add from cb0/cb1 yields the exact expected
  GLSL. ctest 23/23, guest suite 21/21.
- Stage 3b (done, verified): texture sampling + loads. `dcl_resource tN`
  emits sampler declarations from the resource type in opcode bits 11-14
  (sampler2D/sampler3D/samplerCube/sampler2DArray; 1D/buffer/MS/raw refused
  — GLSL ES 3.0 has no faithful forms); `sample` emits texture(tN, uv.xy/
  uv.xyz) for the declared type; `ld` emits texelFetch(tN, ivec2(addr.xy),
  int(addr.z)) for 2D. uv/address operands must have identity swizzles (real
  fxc output always does); everything else is refused honestly. Verified:
  decode+emit of dcl_resource/sampler + sample + ld yields the exact expected
  GLSL, and a 1D resource is refused. ctest 23/23, guest suite 21/21.
- Stage 3c (done, verified): sample variants + comparison sampling.
  sample_b/sample_lod emit texture()/textureLod() with a scalar bias/lod
  (replicated-swizzle operands read via their single component, as fxc
  emits); sample_grad emits textureGrad(uv, ddx.xy, ddy.xy); sample_c emits
  vec4(texture(tN_shadow, vec3(uv.xy, ref))) (scalar result replicated) using
  a sampler2DShadow declared from dcl_sampler's comparison mode (opcode bits
  11-14) via a pre-scan, with the plain sampler2D kept for non-comparison
  uses of the same texture. sample_c against a default sampler is refused.
  All variants are 2D-only for now (honest). Verified: exact GLSL strings for
  all four variants + shadow declaration, refusal of comparison-with-default-
  sampler. ctest 23/23, guest suite 21/21.
- Stage 3d (done, verified): end-to-end DXBC->GLSL + D3D11 wiring.
  `dxbc_to_glsl()` — one call from raw .cso bytes to GLSL text (dxbc_parse ->
  sm4_decode -> sm4_emit_glsl, honoring the SHDR token-count header); verified
  end-to-end in test_shader_translator with a real-format container, and a
  container whose shader is outside the subset fails the whole translation.
  ID3D11Device::CreateVertexShader (vtbl 12) / CreatePixelShader (vtbl 15)
  are now wired through it: the shader object stores the emitted GLSL +
  translated flag (false for unsupported bytecode; the call still succeeds so
  guests keep running), readable via d3d11_shader_get_glsl() for the future
  pipeline-layout binding. test_d3d11_shaders drives the REAL ms_abi vtable
  with a real DXBC blob and asserts the GLSL, plus garbage-bytecode refusal.
  ctest 24/24, guest suite 21/21.
- Stage 3e (done, verified): complete compilable shaders + REAL compiler
  validation. `sm4_emit_glsl_shader()` emits a standalone shader (#version
  310 es + precision (incl. samplers) + global declarations + void main()).
  cbuffers became std140 UBOs (`uniform cbN_b { vec4 data[M]; } cbN;`, UBO
  bindings 16+ to avoid sampler bindings 0-15) and samplers carry
  layout(binding=N) — both required by glslang's Vulkan/SPIR-V profile. The
  emitter also now produces GLSL ES-valid conditions (any(notEqual(..))) and
  passes per-pass state (resource types, comparison samplers, shadow usage)
  between the declaration and body passes. Validation: built glslang
  (KhronosGroup/glslang, ENABLE_OPT=OFF) and made the tests shell out to it
  when GLSLANG_VALIDATOR is set (scripts/verify_shader_glsl.sh). ALL FIVE
  emitted shaders (ALU, control flow, cbuffers, textures, sample variants +
  shadow) COMPILE to SPIR-V with zero errors. The compiler caught real bugs
  this round: #version placement, missing sampler binding/precision, vector
  != operator (ES forbids it), plain uniforms (Vulkan forbids them). This is
  the first end-to-end acceptance: DXBC -> GLSL -> SPIR-V, compiler-verified.
  ctest 24/24, guest suite 21/21, glslang validation green.
- Stage 4a (done, verified): real COM vtable layouts + pipeline-state
  plumbing. Extraction from wine's d3d11.idl/dxgi.idl (byte-exact with the
  Windows SDK) exposed that the CONTEXT vtable was missing the 4 inherited
  ID3D11DeviceChild methods: OMSetRenderTargets is 33 (was 29),
  ClearRenderTargetView 49 (was 46), RSSetViewports 43 (was 40), ClearState
  111, Flush 112; VSSetShader = 11, PSSetShader = 9, IASetInputLayout = 17;
  swapchain GetDesc was 7 (real 12), Present/GetBuffer already correct.
  Fixed all slots, filled Draw/DrawIndexed no-ops, and added
  CreateInputLayout (device 11) capturing D3D11_INPUT_ELEMENT_DESC arrays
  (semantics+formats) plus VSSetShader/PSSetShader/IASetInputLayout binding
  into a pipeline snapshot (d3d11_context_pipeline_snapshot) for the future
  Vulkan pipeline builder. The d3d clear guest was updated from the old
  wrong offsets (46/29) to the real Windows ones (49/33). Verified:
  test_d3d11_shaders drives CreateInputLayout + the three setter slots
  through the REAL vtables and checks the snapshot; guests 21/21.
  ctest 24/24, guest suite 21/21, glslang validation green.
- Stage 4b (done, verified): D3D11-state -> Vulkan pipeline mapping.
  `pipeline_map` (pure logic, no device needed): DXGI_FORMAT -> VkFormat for
  vertex inputs (real VK_FORMAT constants; typeless/depth/compressed/unknown
  refused as 0) + per-format sizes; build_vertex_input computes
  VkVertexInputBinding/AttributeDescription equivalents (one binding per
  input slot, stride = max element end, D3D11_APPEND_ALIGNED_ELEMENT at
  4-byte alignment like the D3D10/11 runtime, per-instance slots flagged);
  build_descriptor_layout matches the emitter's binding scheme (samplers
  0-15, cbuffers 16-31, shadow samplers 32-47) so the GLSL->SPIR-V the
  emitter produces and the descriptor set the pipeline builder will write
  agree by construction. Also fixed a latent Vulkan-invalid emitter output:
  tN_shadow shared binding N with the plain tN sampler; shadows moved to
  32+N. test_pipeline_map (25th ctest): format spot checks against real
  VkFormat values, append-aligned + per-instance vertex math, descriptor
  layout + collision check, depth-format refusal. ctest 25/25, guest suite
  21/21, glslang validation green.
- Stage 4c (done, verified): in-process GLSL -> SPIR-V via the glslang
  LIBRARY. `shader_compile` links glslang statically (CMake option
  PAPAYA_GLSLANG_ROOT + SPIRV-Headers; without it the API refuses cleanly):
  compile_glsl_to_spirv() drives glslang's C++ API (ES 3.10, Vulkan client,
  SPIR-V 1.0) and dxbc_to_spirv() closes the whole chain in one process:
  DXBC container -> decode -> GLSL -> SPIR-V words (168 words for the test
  shader). The D3D11 shader objects now carry the compiled SPIR-V
  (d3d11_shader_get_spirv), with CreateVertexShader compiled as vertex stage
  and CreatePixelShader as fragment stage, so the Vulkan pipeline builder
  gets VkShaderModule-ready binaries directly from Create*Shader. test_
  shader_compile (26th ctest): SPIR-V magic + word count for fragment and
  vertex, clean refusal for garbage bytecode and for missing-glslang builds.
  ctest 26/26, guest suite 21/21, glslang validation green.
- Stage 4d (done, verified): device-bound VkPipeline creation.
  `vulkan_pipeline` builds a real graphics pipeline from the D3D11 state the
  whole chain captures: VS/PS SPIR-V from the shader objects (4c),
  vertex-input bindings/attributes + descriptor set layout from pipeline_map
  (4b), and a dedicated presentable render pass on the swapchain color
  format. The builder is thin glue (shader modules, pipeline layout,
  render pass, fixed-function state: triangle list, fill, no cull,
  CCW front face, dynamic viewport/scissor, optional alpha blend).
  test_vulkan_pipeline (27th ctest) runs headless end-to-end: own
  VkInstance + first graphics device (lavapipe/llvmpipe), compiles a real
  VS+PS from DXBC in-process, builds the pipeline state from an input
  layout + sampler + cbuffer, and creates + destroys the VkPipeline,
  VkRenderPass, VkPipelineLayout and VkDescriptorSetLayout; skips cleanly
  (exit 0) when no Vulkan device exists so CI stays hermetic. On this host
  it creates a REAL pipeline. ctest 27/27, guest suite 21/21, glslang
  validation green.
- Stage 4e (done, verified): offscreen record/execute + FIRST RENDERED FRAME.
  `render_offscreen` runs the whole hardware path: vertex buffer upload,
  descriptor pool/set with UBO writes (binding b at 16*(b-16) in a 64KB
  scratch UBO) and a 2x2 test texture, offscreen render target + framebuffer,
  command recording (render pass with blue clear, pipeline + descriptors +
  vertex buffer + draw), submission, and image->buffer readback. Two real
  bugs found while getting it to render: (1) the GLSL emitter declared
  vN/oN as plain globals, so SPIR-V had no shader interface - they are now
  `layout(location = N) in/out vec4` and vertex shaders emit
  `gl_Position = o0` (dxbc_to_glsl_stage threads the stage through);
  (2) dynamic viewport/scissor silently drew nothing on both llvmpipe and
  the Intel driver - the builder now bakes a static viewport matching the
  D3D11 target size (which matches D3D11 state semantics anyway).
  test_vulkan_pipeline renders a real triangle through the translated
  pipeline and asserts the pixels (red triangle on blue clear). ctest 27/27,
  guest suite 21/21, glslang validation green.
- Stage 4f (done, verified): swapchain GPU-render path. render_pipeline was
  generalized to target an EXTERNAL image (own offscreen target + readback
  when target=0), so the same verified record/execute core can render into
  swapchain images. VulkanSwapchain::render_and_present() then does the
  swapchain integration: per-spec cached VkPipeline/layout/descriptor-
  layout/render-pass (spec identity = the SPIR-V pointers owned by the D3D11
  shader objects), lazily created image views + framebuffers per swapchain
  image, acquire -> render_pipeline into the image -> present. Also exposed
  device()/physical_device()/surface_format() for the pipeline builder.
  Honest scope: presenting through a real surface still needs a visible
  X11 window (not exercisable headless), so the swapchain test verifies the
  headless contract: accessors are 0 and render_and_present refuses cleanly
  before initialization; the rendered-frame acceptance stays on the
  offscreen path (Stage 4e). ctest 27/27, guest suite 21/21, glslang
  validation green.
- Stage 4g (done, verified): win32 draw glue. The D3D11 layer now captures
  the vertex input a real app sets up: ID3D11Device::CreateBuffer (vtbl 3,
  D3D11_BUFFER_DESC), ID3D11DeviceContext::Map/Unmap (14/15, the guest
  writes into the host storage), and IASetVertexBuffers (18, slot 0:
  buffer + stride + offset). `d3d11_context_vertex_data` hands the captured
  data out; `d3d11_context_draw_vertices` builds the PipelineSpec from the
  snapshotted state (VS/PS SPIR-V, input layout -> vertex input via
  pipeline_map with the IASetVertexBuffers stride honored, bound vertex
  buffer) and drives VulkanSwapchain::render_and_present. sc_present now
  tries the GPU pipeline path first when PAPAYA_VULKAN=1 and falls back to
  the CPU upload blit (resource-using shaders come back false until Stage
  3f's resource binds). Verified through the REAL vtables: CreateBuffer/
  Map/Unmap/IASetVertexBuffers roundtrip triangle vertices into the
  snapshot getter, and draw_vertices refuses cleanly with an uninitialized
  swapchain. ctest 27/27, guest suite 21/21, glslang validation green.
- Stage 3 (next): switch/case (int value representation), integer ops,
  resinfo/gather + 3D/array sample variants + DCL input-signature coupling
  (inputs/outputs from ISGN/OSGN instead of fixed vN/oN), then D3D11-state->
  Vulkan pipeline mapping (OMSetRenderTargets->attachments, shaders->pipeline
  stages, descriptors from the UBO/sampler bindings the emitter now emits).

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