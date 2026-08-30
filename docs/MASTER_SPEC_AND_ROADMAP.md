# Papaya — Complete Wineless Compatibility Platform (Master Spec)

**Status: Adopted 2026-08-30. Source: user-provided master specification (see
`COMPATIBILITY_MASTER_SPEC_Raw.md` for the verbatim text).**

## 0. The strategic reframe (read first)

The goal is **not** "remove Wine from Papaya". The goal is:

> Make Papaya's existing Wine-less compatibility runtime as capable,
> performant, and complete as Wine/Proton — **without depending on Wine at all.**

That changes the engineering plan substantially: build a coherent Windows
execution environment first, then layer the gaming stack — not a collection
of hacks that happens to launch games.

### Absolute architectural rule
Papaya must never depend on Wine: no Wine executables, DLLs, loader, server,
prefix, registry implementation, NTDLL, Win32 implementation, or any
Wine-derived runtime dependency. Papaya independently implements equivalent
behavior. It may (and should) use Linux/system libraries (Vulkan, PipeWire,
FreeType, HarfBuzz, FFmpeg, io_uring, mesa) as backends.

### Honesty contract (standing, agreed with the user)
State the honest ceiling plainly (Wine ≈ 15-20M LOC/30yr, DXVK ≈ 2M LOC,
full GPU-D3D translation is a multi-year effort; per-thread
`__declspec(thread)` and full MSVC SEH are toolchain-limited). Then deliver
**real, verified increments** instead of grinding under a false banner.
Never claim work is done that isn't; never ship known-broken state (revert to
last green commit instead). No fake stubs masquerading as implementations.

## 1. Architecture target

```
                     PAPAYA
        ┌──────────────┴──────────────┐
   Windows Apps                  Windows Games
        │                              │
        ▼                              ▼
  ┌───────────┐                  ┌──────────────┐
  │ PE Loader │                  │ Game Runtime │
  └─────┬─────┘                  └──────┬───────┘
        │                               │
        ▼                               ▼
  ┌─────────────────────────────────────────────┐
  │            PAPAYA NT RUNTIME                │
  │ Win32/NT/COM/Registry  Processes/Threads/   │
  │ Memory  Filesystem/Networking/Security      │
  │ Exceptions/Synchronization                  │
  └───────────┬────────────────┬───────────────┘
       ┌──────┴─────┐    ┌─────┴──────┐
       ▼            ▼    ▼            ▼
    Vulkan      PipeWire     Linux
       └──────────┼──────────┘
                  ▼
            LINUX / ANDROID
```

Nothing underneath Papaya is allowed to secretly call Wine.

## 2. Compatibility domains (target matrix)

| Domain    | Target |
|---|---|
| PE/COFF   | Full PE32/PE32+ loader: sections, imports/exports, relocations, TLS, delay-load, bound imports, resources, manifests, activation contexts, API-sets, forwarded/ordinal exports, CFG metadata, exception metadata, debug dirs, authenticode, section mapping, ASLR, DEP/NX, large-address-aware |
| x86/x64   | Native execution |
| ARM64     | Native ARM64 + x86→ARM64 high-performance JIT/translation (decoder → IR → optimizer → regalloc → codegen → code cache → branch linking → SMC detection) |
| NT        | Comprehensive NT semantics; ntdll-equivalent + Rtl* family |
| Win32     | kernel32/kernelbase/user32/gdi32/shell32/advapi32/ole32/oleaut32/ws2_32/winmm/dinput/xinput… |
| Memory    | Windows-compatible VM semantics (VirtualAlloc/Free/Protect/Query, MEM_* states, PAGE_* protections, guard pages, heap/LFH-like) |
| Threads   | Windows thread/TEB semantics, TLS/FLS, fibers, APCs, thread priorities/affinity |
| Processes | Windows process/PEB semantics, jobs, environment, command line, CreateProcess |
| DLLs      | Loader, imports/exports/delay-load, DLL search order, known DLLs, API sets, DLL notifications, DllMain, TLS callbacks |
| Exceptions| SEH/VEH/VCH, hardware + software exceptions, x64 .pdata/.xdata RUNTIME_FUNCTION/UNWIND_INFO, x86 FS:[0], crash/minidump infra |
| Sync      | Events, mutexes, semaphores, critical sections, SRW, condvars, WaitOnAddress, WaitForMultipleObjects, futex/eventfd/epoll/timerfd/io_uring backends |
| Registry  | Full Windows registry model (HKLM/HKCU/HKCR/HKU, types, notifications, transactions, 32/64 views), persistence |
| Filesystem| NT/DOS/UNC/reparse semantics, case-insensitive lookups, sharing modes, locking, attributes, ADS, named pipes, memory-mapped files |
| COM       | Full COM/OLE infrastructure (apartments, activation, marshalling, proxy/stub, RPC, typelibs, automation, VARIANT/BSTR/SAFEARRAY) |
| Security  | Tokens, SIDs, ACLs, security descriptors, privileges, impersonation, integrity levels |
| Services  | Windows service semantics (SCM) |
| Networking| Winsock/WinHTTP/WinINet, IOCP overlapped sockets, DNS, TLS |
| Graphics  | D3D9/10/11/12 + DXGI, shaders (DXBC/DXIL → Papaya IR → SPIR-V → Vulkan), swapchains, fences, GPU memory |
| Audio     | XAudio2/WASAPI/DirectSound/WinMM, backends PipeWire/ALSA/AAudio |
| Input     | XInput/DirectInput/Raw Input/HID, evdev/hidraw/libinput/Android backends |
| Media     | Media Foundation / DirectShow / codecs (FFmpeg/GStreamer) |
| Printing  | WinSpool |
| Crypto    | BCrypt/CNG/Crypt32, certificate stores, TLS |
| Shell     | Shell32/Explorer APIs, known folders |
| Installer | MSI / Windows installer behavior |
| Steam     | Steamworks compatibility (dry: no Goldberg-style injection/deletion; do not modify a game's legitimate Steam libraries) |
| Anti-cheat| User-mode + carefully scoped kernel compatibility; honest boundary (cannot solve kernel drivers purely in userspace) |
| Debugging | Windows-compatible crash/debug infrastructure, minidumps, .papaya-crash bundles |

## 3. Subsystem checklist (grand master list, condensed)

Core runtime (papaya-runtime/ loader, nt, process, thread, memory, handles,
objects, exceptions, synchronization, io, filesystem, registry, security,
environment, dll, tls, ipc, services, diagnostics) · PE loader · process
model · PEB/TEB (+LDR data, RTL_USER_PROCESS_PARAMETERS) · NT object manager
· handle manager · virtual memory · heap manager · synchronization engine ·
exception engine · TLS (incl. per-thread eventually) · DLL runtime · API-set
architecture (data-driven) · kernel32/kernelbase · ntdll-equivalent · Rtl*
· user32 windowing · window-manager abstraction (Wayland/X11/Android/headless)
· GDI/GDI+ · DirectWrite (FreeType/HarfBuzz) · Direct2D · COM subsystem ·
RPC · registry · Windows filesystem namespace + path manager · I/O manager
(async/overlapped/IOCP) · networking · TLS/crypto · security · services ·
task scheduler · shell · environment · installer compat (papaya-msi) ·
multimedia · audio · input · graphics master stack (D3D9/10/11/12 + DXGI +
shader compiler + shader cache) · GPU sync/memory · vendor support (NVAPI-
compatible surface, AMD, Intel; capability-based, never fake) · Wayland/X11 ·
HDR/VRR/FSR/frame pacing · VR (OpenXR/OpenVR) · ARM64 translation subsystem ·
SIMD translation · CPU feature virtualization (CPUID/XGETBV) · Steam layer ·
save manager · DRM compatibility (legitimate platform behavior only) ·
anti-cheat · driver-model abstraction (SetupAPI/DeviceIoControl) · Bluetooth
(BlueZ) · camera · printing · accessibility · DPI/scaling · localization/NLS
· time model · power management · WMI-ish management APIs · event log ·
debugger · crash dumps · compatibility DB (declarative, not hardcoded hacks)
· app profiler · runtime configuration per title · environment isolation
(Papaya env, not Wine prefix) · global runtime caches (shader/JIT/font) ·
translation optimizer (interpreter → baseline JIT → optimizing JIT) ·
async-everything · thread scheduler · MMCSS · named pipes · shared memory ·
IPC · clipboard · drag&drop · game controller layer · Steam Deck · Android ·
Snapdragon optimization · Raspberry Pi · hardware detection/capability DB ·
benchmarks · regression testing (commit-level, auto) · Windows reference
testing · API conformance suite (organized per DLL) · fuzzing (PE, shader
bytecode, MSI, paths…) · security sandbox (seccomp/Landlock/namespaces/cgroups
optionally) · resource control · opt-in telemetry · automatic crash diagnosis
· workaround generator (validated) · AI-assisted compat engineering (dev
mode) · trace engine + replay · determinism tooling · structured logging ·
CLI (papaya run/install/inspect/diagnose/benchmark/trace/debug/env/cache/
doctor/update) · GUI launcher · drag-and-drop install · dependency manager ·
.NET interop strategy · WinRT/UWP/MSIX · WebView2/Electron/browser ·
Office/creative/dev-tool testing · engine matrix (Unity/Unreal/GameMaker/
Godot/Source/RE/Frostbite/Decima) · anti-regression game matrix · Proton
feature-parity matrix · performance goals (<5% CPU overhead typical x86-64,
<10% worst-case; frametime stability over avg FPS; millisecond startup) ·
memory goals (shared immutable runtime, COW envs, shared caches) · hot-path
profiling discipline · ABI stability (Papaya Runtime/Graphics/Translation/
Plugin ABIs) · plugin system · platform abstraction · build system (CMake+
Ninja, CI, sanitizers, LTO/PGO) · compiler matrix (GCC/Clang/NDK) · CI matrix
(x86-64, ARM64, SteamOS, Android) · ABI tests · docs · developer SDK ·
compatibility reporting (per-domain scores) · capability negotiation ·
version emulation (Win7/8.1/10/11) · registry compat profiles · locale
profiles · virtual hardware (coherent, capability-based, not random spoofing)
· game-specific patches (isolated, declarative) · regression detection ·
golden tests · differential testing (Windows vs Papaya) · API coverage
dashboard (implemented/tested/passing/partial/stubbed/missing per DLL) ·
stub lifecycle (owner, test, priority, target apps, must disappear) ·
priority system (top-1000-APIs by real usage; top-100 apps; top-20 engines) ·
timer subsystem · APC subsystem · fibers · context switching · WOW64-like
architecture · x86-on-ARM + WOW64 · ARM64EC · memory coherency for translated
code · signal↔exception mapping (SIGSEGV→AV, SIGILL→ILL, SIGFPE→#DE,
SIGTRAP→breakpoint) · Linux syscall abstraction (Papaya Host ABI) · Android
JNI boundary · Android surface integration · thermal management · handheld
frame pacing · battery-aware shader compilation · background cache building ·
portable packaging (AppImage/Flatpak/Steam compat tool/APK) · versioning ·
stable 1.0 ABI · update system · offline mode · reproducible builds ·
security hardening · sandboxed parsing · archive handling (ZIP/7z/RAR/ISO
with zip-bomb/path-traversal protection) · game detection (Steam/GOG/Epic/
EA/Ubisoft/Battle.net) · launcher detection · multi-executable apps · child
process inheritance · environment propagation · process-injection compat
(CreateRemoteThread/VirtualAllocEx/WriteProcessMemory) · shared DLL state ·
native DLL loading rules · DLL classification · DLL conflict resolver ·
side-by-side assemblies · manifest system · UAC semantics · virtualization ·
file locking semantics · case handling · NT namespace · named sync objects ·
shared memory naming · registry notifications · filesystem notifications
(ReadDirectoryChangesW via inotify) · Windows console (ConPTY, VT, UTF-16,
code pages) · headless mode · server-app testing · networking performance ·
DNS · HTTP · cert stores · DPAPI · credential APIs · environment identity
(stable per-env) · machine identity · registry boot (SYSTEM/SOFTWARE/…)
· boot-like init (runtime→registry→services→COM→graphics→audio→input→app) ·
service dependency graph · lazy services · COM activation DB (CLSID/ProgID/
AppID/TypeLib/Interface) · COM DLL servers (DllGetClassObject) · COM EXE
servers · automation (IDispatch/OLE) · graphics interop (GDI/D2D/D3D/DXGI/MF)
· GPU shared resources · video decode (H.264/H.265/AV1/VP9) · rumble/haptics
· motion sensors · overlay system · performance profiles · dynamic perf ·
game-specific optimization · Steam Deck integration · controller-first UI ·
Android launcher · metadata/covers · game database · compatibility scores
(native/excellent/good/playable/broken + evidence links) · community reports
· automatic bug reports · reproduction packages · compatibility bisector ·
runtime feature flags · safe fallbacks (D3D12→D3D11) · GPU cap fallback ·
shader fallback · device-loss handling (DXGI_ERROR_DEVICE_REMOVED/RESET)
· graphics/audio/network recovery · suspend/resume · Android lifecycle ·
save safety (atomic/journal/backup/rollback) · crash-safe env (transactional
registry) · atomic install · cache corruption recovery · runtime self-test ·
full diagnostic mode · developer mode · API/GPU/JIT tracing · compat test CLI
· benchmark CLI · regression CLI · env CLI · cache CLI · package CLI.

### Engine/ABI details to remember
- Guest testing: real mingw guests in /tmp/pvx; cross-compile with
  x86_64-w64-mingw32-gcc; harness runv.cpp links build/src/*/libpapaya-*.a.
- Verify with asymmetric colors (red/blue, not green) for pixel round-trips.
- HLE fns: PAPAYA_MS_ABI; register in register_function; registration is
  later-wins; never stub over a real impl.
- Always relink the run harness after rebuilding libs (stale `run` has
  caused repeated false failures / misattributed crashes).

## 4. Phased delivery plan

```
PHASE 0  Architecture cleanup (repo audit below)
PHASE 1  PE + execution
PHASE 2  NT runtime
PHASE 3  Win32
PHASE 4  Filesystem + registry + process model
PHASE 5  COM + RPC
PHASE 6  Graphics foundation
PHASE 7  D3D9/10/11
PHASE 8  D3D12
PHASE 9  Audio/Input/Media
PHASE 10 Steam
PHASE 11 x86/WOW64
PHASE 12 ARM64 JIT
PHASE 13 Android/handheld optimization
PHASE 14 Compatibility laboratory (DB, profiler, differential tests)
PHASE 15 Massive application/game matrix
PHASE 16 Performance optimization
PHASE 17 Security hardening
PHASE 18 Papaya 1.0
```

The single most important strategic difference from Proton: build a coherent
Windows execution environment first; don't make Papaya a collection of hacks
that happens to launch games. Use Proton as the feature **benchmark**, not as
an implementation dependency.

## 5. Phase 0 — Architecture cleanup (immediate work when resuming)

Concrete repo audit + cleanup tasks (2026-08-30 status):

1. **Restructure toward `papaya-runtime/` layering** — today compatibility
   logic is scattered through `src/win32/src/win32_api_hle.cpp` (one ~6k-line
   file with every family) and the orchestrator. Split into
   loader / nt / process / thread / memory / handles / objects / exceptions /
   synchronization / io / filesystem / registry / security / dll / tls.
2. **API coverage dashboard** — regenerate `docs/GAME_API_COVERAGE.md` from
   the mingw import libs (37 DLLs, 12,695 exports, ~351 implemented ≈ 2%);
   add implemented/tested/passing/partial/stubbed/missing columns per DLL.
3. **Kill fake stubs systematically** — audit generic_stub registrations
   (currently ~9); each must gain an owner + test + target app, prioritized
   by the top-1000-APIs ranking.
4. **Host ABI abstraction** — isolate Linux syscalls behind a Papaya Host ABI
   so Android portability is a backend, not a rewrite.
5. **Structured logging** — replace the stdout-ANSI logger with leveled,
   structured channels (ERROR/WARN/INFO/DEBUG/TRACE/PERF/API/GPU/JIT).
6. **Diagnostics** — crash path already terminates properly since 11806ad;
   add minidump-style capture (registers, modules, stack, exception record)
   into a `.papaya-crash` bundle.
7. **CI** — commit-level: build (x86-64), ctest, guest-exe suite
   (/tmp/pvx set + verify script), no-wine greps. Then ARM64/Android matrix.
8. **Fuzzing** — libFuzzer/AFL++ harnesses for PE parsing first (highest
   hostile-input risk); then registry, manifests, shader bytecode.

### Known immediate issues found in-session (for Phase 1 queue)
- Guest-call invocation of __try1-style filter functions resumes at the
  filter entry without a call frame, so custom filter fns print but crash on
  return. Resume-at-handler only works for __except-block continuations.
  Document; implement proper `__C_specific_handler` semantics
  (call filter fn, jump to block) later.
- `register_function` count: ~750 HLE registrations; `generic_stub_*` only
  for explicitly listed cases — never register stubs over real impls.
- msvcrt missing imports seen on every guest: IsDBCSLeadByteEx,
  ___lc_codepage_func, ___mb_cur_max_func, _lock, _unlock, fflush,
  localeconv, strerror (all msvcrt). These show as unresolved-import
  warnings and fall to fallback_iat_stub; implementing them (host-crt-backed)
  would remove boot-time noise and enable full printf-family behavior.

## 6. Immediate next tasks (when resuming, in order)

1. Finish Phase 0 item 1: sketch `papaya-runtime/` layout and move the first
   family (filesystem + registry) out of win32_api_hle.cpp.
2. Implement the 8 unresolved msvcrt imports above (host-backed) so every
   guest boots clean; add a boot-noise regression check to the verify script.
3. Regenerate GAME_API_COVERAGE.md with the implemented/tested columns.
4. Build the top-1000-APIs ranking script from the mingw import libs +
   repo usage data; pick the next HLE families by rank (expect: getaddrinfo/
   WSA*, GetSystemInfo, GlobalMemoryStatusEx, timeGetTime family already in).
5. Every feature: build (`ninja -j1`), ctest 18/18, guest exe exit 0,
   commit, push. Honesty contract applies to claims about D3D/GPU.