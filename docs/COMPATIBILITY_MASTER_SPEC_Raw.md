# Papaya — Complete Wineless Compatibility Platform (RAW SPEC)

Verbatim user-supplied master specification, captured 2026-08-30. The
distilled/actionable form lives in `MASTER_SPEC_AND_ROADMAP.md`. This file is
the reference text to follow for scope and priorities.

---

## Reframe

The goal is not "Remove Wine from Papaya." It's: make Papaya's existing
Wine-less compatibility runtime as capable, performant, and complete as
Wine/Proton, without depending on Wine at all. That changes the engineering
plan substantially.

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
  │ Win32 / NT / COM / Registry                 │
  │ Processes / Threads / Memory                │
  │ Filesystem / Networking / Security          │
  │ Exceptions / Synchronization                │
  └───────────┬────────────────┬───────────────┘
       ┌──────┴─────┐    ┌─────┴──────┐
       ▼            ▼    ▼            ▼
    Vulkan      PipeWire     Linux
       └──────────┼──────────┘
                  ▼
            LINUX/ANDROID
```

And nothing underneath Papaya should secretly call Wine.

### What Papaya ultimately needs to match (domains)

| Domain | Target |
|---|---|
| PE/COFF | Full PE32/PE32+ loader |
| x86 | Native x86 execution |
| x64 | Native x64 execution |
| ARM64 | Native ARM64 |
| x86 → ARM64 | High-performance JIT/translation |
| NT | Comprehensive NT semantics |
| Win32 | Kernel32/KernelBase/User32/etc. |
| Memory | Windows-compatible VM semantics |
| Threads | Windows thread/TEB semantics |
| Processes | Windows process/PEB semantics |
| DLLs | Loader, imports, exports, delay-load |
| Exceptions | SEH/VEH/unwind |
| Synchronization | Events, mutexes, SRW, futex-like paths |
| Registry | Full Windows registry model |
| Filesystem | NT/DOS/UNC/reparse semantics |
| COM | Full COM/OLE infrastructure |
| Security | Tokens/SIDs/ACLs/security descriptors |
| Services | Windows service semantics |
| Networking | Winsock/WinHTTP/WinINet |
| Graphics | D3D9/10/11/12 + DXGI |
| Shaders | DXBC/DXIL → Papaya IR → Vulkan |
| Audio | XAudio2/WASAPI/DirectSound |
| Input | XInput/DirectInput/Raw Input |
| Media | Media Foundation / codecs |
| Printing | Winspool |
| Crypto | BCrypt/CNG/Crypt32 |
| Shell | Shell32 / Explorer APIs |
| Installer | MSI / Windows installer behavior |
| Steam | Steamworks compatibility |
| Anti-cheat | User-mode + carefully scoped kernel compatibility |
| Debugging | Windows-compatible crash/debug infrastructure |

## Absolute architectural rule

Papaya must never depend on Wine. That means no: Wine executable, Wine DLLs,
Wine loader, Wine server, Wine prefix, Wine registry implementation, Wine
NTDLL, Wine Win32 implementation, Wine-derived runtime dependency that
requires Wine. Papaya can independently implement equivalent behavior.

```
             WINDOWS PROGRAM
                    │
                    ▼
             ┌──────────────┐
             │ PAPAYA LOADER│
             └──────┬───────┘
                    │
                    ▼
           ┌──────────────────┐
           │ PAPAYA NT RUNTIME│
           └────────┬─────────┘
                    │
       ┌────────────┼────────────┐
       ▼            ▼            ▼
    WIN32        GRAPHICS       MEDIA
       │            │            │
       ▼            ▼            ▼
   Papaya APIs  Papaya GPU    Papaya Media
       │            │            │
       └────────────┼────────────┘
                    ▼
             Linux / Android
```

## The complete scoped checklist

### 1. Core runtime
Create a proper runtime rather than having compatibility functionality
scattered through the orchestrator. papaya-runtime/: loader, nt, process,
thread, memory, handles, objects, exceptions, synchronization, io,
filesystem, registry, security, environment, dll, tls, ipc, services,
diagnostics. This becomes the foundation everything else uses.

### 2. PE loader
Production-grade Windows executable loader: PE32, PE32+, DLL loading, import
resolution, export resolution, relocations, TLS, delay imports, bound
imports, resources, manifests, activation contexts, load configuration, CFG
metadata, exception metadata, debug directories, Authenticode parsing,
section mapping, ASLR, DEP/NX, large-address-aware, image characteristics,
API-set resolution, forwarded exports, ordinal imports, side-by-side
assemblies, known-DLL behavior. Flow: DOS → PE → Sections → Imports →
Exports → Relocations → TLS → Resources → Exceptions → Manifest → Papaya
Image → Virtual Address Space.

### 3. Windows process model
Process, thread, job, token, handle table, PEB, TEB, environment, command
line, startup information, process parameters. APIs: CreateProcess,
CreateProcessAsUser, OpenProcess, TerminateProcess, ExitProcess,
GetExitCodeProcess, CreateThread, CreateRemoteThread, OpenThread,
TerminateThread, SuspendThread, ResumeThread, GetExitCodeThread, and the
underlying NT equivalents.

### 4. PEB / TEB
Realistic PEB, TEB, PEB_LDR_DATA, RTL_USER_PROCESS_PARAMETERS,
LDR_DATA_TABLE_ENTRY with loaded DLL list, environment, command line, heap,
image base, process parameters, TLS, locale, thread data, exception data.
A lot of software accesses these indirectly through Windows libraries.

### 5. NT object manager
Object model: Process, Thread, Event, Mutex, Semaphore, Section, File,
Directory, Token, Timer, Job, Port, Key, Pipe, IoCompletionPort; NtCreate*,
NtOpen*, NtQuery*, NtSet*, NtDuplicate*, NtClose with a unified handle
system.

### 6. Handle manager
Central subsystem: generation-safe handles, reference counting, inheritance,
duplication, access masks, object lifetime, per-process handle tables,
pseudo-handles, handle auditing, waitable handles.

### 7. Virtual memory
Windows semantics over Linux/Android primitives: VirtualAlloc, VirtualFree,
VirtualProtect, VirtualQuery, VirtualLock, VirtualUnlock, MapViewOfFile,
UnmapViewOfFile, NtAllocateVirtualMemory, NtFreeVirtualMemory,
NtProtectVirtualMemory, NtQueryVirtualMemory, NtMapViewOfSection,
NtUnmapViewOfSection. Support MEM_RESERVE/COMMIT/DECOMMIT/RELEASE/RESET/
TOP_DOWN/LARGE_PAGES; PAGE_READONLY/READWRITE/EXECUTE/EXECUTE_READ/
EXECUTE_READWRITE/GUARD.

### 8. Heap manager
Windows-compatible: HeapCreate, HeapAlloc, HeapReAlloc, HeapFree,
HeapDestroy, GetProcessHeap, LocalAlloc, GlobalAlloc. NT heap, process heap,
private heaps, LFH-like behavior, heap debugging, guard pages, heap
corruption detection. Performance matters enormously.

### 9. Synchronization engine
Mutex, event, semaphore, critical section, SRW lock, condition variable,
WaitOnAddress, WakeByAddressSingle, WakeByAddressAll, futex-style waits,
WaitForSingleObject, WaitForMultipleObjects, MsgWaitForMultipleObjects. Use
efficient Linux primitives underneath: futex, eventfd, epoll, timerfd,
io_uring where semantics permit.

### 10. Exception engine
SEH, VEH, VCH, hardware exceptions, software exceptions, access violations,
illegal instructions, divide by zero, stack overflow, breakpoints, debug
exceptions, C++ exception interoperability. x64: RUNTIME_FUNCTION,
UNWIND_INFO, .pdata, .xdata. x86: SEH FS:[0]. ARM64: Windows ARM64 unwind
semantics.

### 11. Thread-local storage
TLS, FLS, Thread Environment Block, DLL TLS callbacks, TLS index allocation,
Fiber Local Storage.

### 12. DLL runtime
Real DLL loader: LoadLibrary, LoadLibraryEx, FreeLibrary, GetModuleHandle,
GetProcAddress, DLL search order, known DLLs, API sets, forwarders, delay
loading, activation contexts, side-by-side assemblies, DLL notifications,
TLS callbacks, DllMain.

### 13. API-set architecture
Modern apps depend on api-ms-win-*.dll / ext-ms-win-*.dll. Papaya needs a
data-driven API-set resolver: api-ms-win-core-file-l1-2-0 → Papaya
KernelBase implementation.

### 14. Kernel32 / KernelBase
Categories: process, thread, memory, file, console, environment,
synchronization, time, system information, DLL, locale, unicode, atom,
toolhelp, debugging, power, firmware, job objects, IO completion.

### 15. NTDLL-equivalent
papaya-ntdll exposes Windows-compatible exports while internally calling
Papaya's runtime. Subsystems: loader, process, thread, memory, io, object,
registry, sync, exception, security, time, section, syscall, environment,
rtl. The Rtl* family needs serious coverage.

### 16. RTL
RtlAllocateHeap, RtlFreeHeap, RtlInitUnicodeString,
RtlUnicodeStringToAnsiString, RtlCompareUnicodeString, RtlGetVersion,
RtlRandomEx, RtlEnterCriticalSection, RtlLeaveCriticalSection,
RtlAcquireSRWLock*, RtlRaiseException, RtlCaptureContext,
RtlLookupFunctionEntry, etc.

### 17. User32
CreateWindowEx, DestroyWindow, ShowWindow, UpdateWindow, DefWindowProc,
DispatchMessage, TranslateMessage, PeekMessage, GetMessage, PostMessage,
SendMessage, RegisterClass; keyboard, mouse, clipboard, cursors, icons,
menus, dialogs, hooks, timers, DPI, monitor enumeration, window styles,
focus, activation, IME, raw input.

### 18. Window manager abstraction
Do not make User32 directly depend on X11. Papaya Window Backend: Wayland,
X11, Android, headless. User32 → Papaya Window System → backend.

### 19. GDI/GDI+
HDC, HBITMAP, HBRUSH, HPEN, HFONT, HRGN, HPALETTE; BitBlt, StretchBlt,
AlphaBlend, TextOut, DrawText, CreateFont, SelectObject,
CreateCompatibleDC. Eventually GDI+.

### 20. DirectWrite
Font fallback, font collections, glyph shaping, text layout, DPI, Unicode,
OpenType, variable fonts. Backend: FreeType + HarfBuzz.

### 21. Direct2D
ID2D1Factory, ID2D1RenderTarget, ID2D1Device, ID2D1DeviceContext, brushes,
bitmaps, geometries, text, effects; Vulkan-backed where appropriate.

### 22. COM
papaya-com/: runtime, apartments, activation, registry, marshalling, proxy,
stub, RPC, typelib, automation. IUnknown, IClassFactory, IDispatch,
ITypeInfo, VARIANT, BSTR, SAFEARRAY. STA, MTA, COM initialization, COM
apartments, marshalling, out-of-process servers.

### 23. RPC
Enough Windows RPC for COM, DCOM, services, system components, applications.

### 24. Registry
Complete virtual registry: HKLM, HKCU, HKCR, HKU, HKCC. Keys, values, types,
permissions, notifications, transactions, symbolic links, volatile keys,
registry reflection where applicable. Storage can be custom binary or SQLite
— but the API must not expose SQLite semantics.

### 25. Windows filesystem
Virtual Windows filesystem namespace: C:\, \\server\share,
\Device\HarddiskVolume..., \\?\..., \\.\.... Handle case-insensitive lookup,
case-preserving filenames, sharing modes, mandatory-ish Windows locking
semantics, file attributes, timestamps, reparse points, junctions, symlinks,
alternate data streams, sparse files, memory-mapped files, named pipes.

### 26. Windows paths → Linux paths
Dedicated Papaya Path Manager. Never scatter path conversion throughout the
runtime.

### 27. I/O manager
IRP-like internal model, async I/O, overlapped I/O, IOCP completion ports,
cancellation, file locking, named pipes. Backend: io_uring, epoll, eventfd,
Linux AIO.

### 28. Networking
WSAStartup, socket, connect, bind, listen, accept, send, recv, select,
WSAPoll, IOCP overlapped sockets, Winsock extensions, DNS, IP Helper,
WinHTTP, WinINet, URLMon, Schannel.

### 29. TLS / cryptography
BCrypt, NCrypt, Crypt32, CryptoAPI, certificate stores, X.509, TLS, random
generation, hashing, AES, RSA, ECC, HMAC. Use strong host libraries.

### 30. Security subsystem
SID, ACL, ACE, security descriptors, access tokens, privileges,
impersonation, integrity levels, UAC semantics; map to Linux security where
possible.

### 31. Services
Papaya Service Control Manager: CreateService, OpenService, StartService,
ControlService, DeleteService, QueryServiceStatus; dependencies, startup
types, service accounts, service lifecycle, service recovery.

### 32. Task Scheduler
Scheduled tasks, triggers, startup tasks.

### 33. Windows shell
ShellExecute, ShellExecuteEx, SHGetFolderPath, SHGetKnownFolderPath,
SHGetSpecialFolderPath, IShellItem, IShellFolder.

### 34. Environment
PATH, PATHEXT, TEMP, TMP, APPDATA, LOCALAPPDATA, PROGRAMDATA, USERPROFILE,
SystemRoot, windir, with per-environment isolation.

### 35. Installer compatibility
MSI, Windows Installer APIs, setup executables, bootstrapper behavior,
registry installation, uninstallers. Consider papaya-msi rather than
Wine's msiexec.

### 36. Multimedia
Media Foundation, DirectShow, Windows Media APIs, MFTransform, Source
Reader, Sink Writer. Backend: FFmpeg, GStreamer, PipeWire.

### 37. Audio
XAudio2, WASAPI, MMDevice, DirectSound, WinMM, AudioClient,
AudioRenderClient, AudioCaptureClient. Backend: PipeWire, ALSA, AAudio (for
Android).

### 38. Input
XInput, DirectInput, Raw Input, HID, keyboard, mouse, gamepads, touch, pen.
Backends: evdev, hidraw, libinput, Android input.

### 39. Graphics master stack
```
    Direct3D
        │
   ┌────┼────┐
   ▼    ▼    ▼
 D3D9  D3D11 D3D12
   │    │    │
   └────┼────┘
        ▼
  PAPAYA GPU IR
        │
        ▼
     Vulkan
        │
        ▼
   GPU driver
```

### 40. D3D9
IDirect3D9, IDirect3DDevice9, textures, buffers, shaders, render states,
samplers, queries, surfaces, swap chains.

### 41. D3D10
Complete API compatibility.

### 42. D3D11 (one of the highest priorities)
Device, context, buffers, textures, views, samplers, shaders, input layouts,
rasterization, depth/stencil, blend, queries, predication, UAV, compute,
multithreading, deferred contexts.

### 43. DXGI
Factory, adapter, output, swap chains, surfaces, formats, present,
fullscreen, HDR, VRR, tearing, multi-monitor.

### 44. D3D12 (extremely difficult, mandatory for modern games)
Device, command queues, command lists, command allocators, descriptor heaps,
resource heaps, resources, fences, root signatures, PSOs, pipeline
libraries, queries, ray tracing, mesh shaders, sampler feedback, enhanced
barriers.

### 45. Shader compiler
HLSL, DXBC, DXIL, SPIR-V pipeline: DXBC/DXIL → Papaya Shader IR →
optimization → SPIR-V → Vulkan.

### 46. Shader cache
Shader hashing, pipeline hashing, GPU-specific cache, driver-version cache,
async compilation, cache eviction, cache validation, persistent storage.

### 47. GPU synchronization
Fences, semaphores, timeline semaphores, events, resource barriers, queue
synchronization, cross-thread synchronization.

### 48. GPU memory
VRAM allocation, host-visible memory, device-local memory, mapped resources,
staging resources, aliasing, residency, eviction, budget reporting.

### 49-51. NVIDIA / AMD / Intel support
Papaya's own GPU capability layer: NVAPI-compatible surface, GPU
enumeration, memory information, power information, feature reporting — but
do not falsely advertise unsupported hardware features. AMD: enumeration,
extensions, memory budget, ray tracing, FSR integration. Intel: Arc, Xe,
integrated GPUs.

### 52-53. Wayland / X11
First-class Wayland (xdg-shell, fractional scaling, HDR, VRR, input,
fullscreen, presentation timing). Maintain X11 backend for legacy apps.

### 54-56. HDR / VRR / FSR / upscaling
HDR10, scRGB, color spaces, HDR metadata, tone mapping, swapchain HDR.
FreeSync, G-Sync-compatible VRR, Wayland VRR. FSR, integer scaling, dynamic
resolution, sharpening, frame pacing — optional runtime features, not forced
hacks.

### 57. Frame pacing
Presentation timing, CPU/GPU synchronization, frame latency, queue depth,
VRR, vsync, triple buffering.

### 58. VR
OpenXR, OpenVR compatibility, VR compositor, motion controllers, HMD
tracking.

### 59-62. ARM64 translation
papaya-translate/: x86, x64, arm64, decoder, IR, optimizer, register
allocator, codegen, cache, signal handling. x86→ARM64 JIT: instruction
decoder, basic blocks, IR, SSA, constant propagation, dead code elimination,
register allocation, instruction selection, code emission, code cache,
branch linking, indirect branch prediction, SMC detection. SIMD translation
(SSE→SSE4.2, AVX, AVX2, FMA, BMI, AES where host supports). Eventually ARM64,
ARM64EC, x64 hybrid, x86-on-ARM.

### 63. CPU feature virtualization
CPUID, XGETBV, feature masks, processor topology, cache information, NUMA;
expose sane virtual hardware.

### 64-65. Steam + save manager
Proper Steam integration layer: AppID, Steamworks, Steam Input, Steam Cloud,
Steam Overlay, Steam Networking, achievements, stats, Remote Play, Steam
Deck integration. Do not modify or delete a game's legitimate Steam
libraries; the README's Goldberg injection/deletion approach must not be the
foundation. papaya_steam_saves → Papaya Save Manager: Steam Cloud, local
saves, backup, restore, versioning, cross-device sync, conflict resolution.

### 66. DRM
Separate Windows API compatibility from DRM compatibility. Reproduce
legitimate platform behavior rather than building around DRM circumvention.

### 67. Anti-cheat
Support legitimate compatibility requirements for user-mode anti-cheats,
service-based anti-cheats, security APIs, process inspection, memory APIs,
driver interfaces. Kernel-level systems need carefully scoped architectural
support and cannot all be solved from ordinary userspace.

### 68-69. Driver model + device APIs
Abstraction for SetupAPI, PnP, device properties, IOCTL, device handles (not
necessarily real Windows kernel drivers). SetupAPI, CfgMgr32,
DeviceIoControl, HID, USB, Bluetooth.

### 70. Bluetooth
Windows Bluetooth APIs over BlueZ.

### 71-72. Camera / Printing
Media Foundation camera APIs, DirectShow camera APIs, Windows Imaging APIs,
backed by Linux/Android camera frameworks. WinSpool, GDI printing, printer
enumeration, print jobs.

### 73-75. Accessibility / DPI / Localization
UI Automation, MSAA, accessibility events, screen readers. DPI awareness,
per-monitor DPI, system DPI, DPI virtualization, font scaling, window
scaling. Unicode, UTF-8, UTF-16, locales, code pages, NLS, time zones,
collation, IME.

### 76-77. Time / Power
FILETIME, SYSTEMTIME, QPC, GetTickCount, GetTickCount64, performance
counters, Windows timezone database (QPC needs careful monotonic behavior).
Battery, power status, sleep/hibernate awareness, display power, thermal
information.

### 78-79. WMI-ish / Event logging
WMI COM management interfaces, performance counters, event logs. Papaya
Event Log implementing Windows-compatible APIs storing data natively.

### 80-81. Debugger / crash dumps
Papaya Debugger: breakpoints, watchpoints, registers, memory, threads,
modules, symbols, stack traces, exceptions, minidumps. Automatic crash:
exception capture → register capture → thread capture → loaded modules →
stack unwind → GPU state → .papaya-crash diagnostic bundle.

### 82-84. Compatibility DB / profiler / no giant hardcoded hacks
Declarative DB: application, version, engine, architecture, APIs, graphics,
audio, input, known issues, workarounds, performance, regressions, test
results. Automatic app profiler before launch: EXE → PE scan → imports →
exports → manifests → CPU requirements → graphics APIs → known dependencies
→ compatibility profile. Replace `if game == X: hack` with requirement →
capability → fallback.

### 85-87. Runtime config / env isolation / global caches
Every title: Papaya Environment with CPU/GPU/display/audio/input/
filesystem/registry/network/graphics/shader/JIT/performance/debugging
profiles. environment/: drive_c, registry, users, temp, programdata,
windows, logs, shader-cache, state — a Papaya environment, not a Wine
prefix. Shared immutable runtime: DLL implementations, shader cache,
translation cache, font cache, pipeline cache.

### 88-90. Caches / optimizer
JIT cache (EXE hash + CPU model + Papaya version + JIT version → translation
cache). Shader cache (game + GPU + driver + Papaya version + shader hash).
Translation optimizer: fast interpreter, baseline JIT, optimizing JIT, hot
block/trace detection, profile-guided optimization.

### 91-93. Async / scheduler / MMCSS / IOCP
Don't block game thread for shader/JIT/fs-indexing/compat-analysis/cache/
pipeline/network init. Understand Windows priority classes, thread
priorities, affinity, ideal processors, CPU groups, NUMA, background mode,
MMCSS; map sensibly to Linux. Windows IOCP → Papaya IO subsystem →
io_uring where possible.

### 95-97. Pipes / shared memory / IPC
CreateNamedPipe, ConnectNamedPipe, CallNamedPipe, WaitNamedPipe + Windows
pipe semantics. CreateFileMapping, OpenFileMapping, MapViewOfFile over Linux
shmem. Shared memory, pipes, sockets, events, mutexes, COM/RPC, shared
handles.

### 98-100. Clipboard / drag&drop / controller layer
Text, HTML, images, files, custom formats, clipboard ownership. Windows
drag/drop translated to Wayland/X11/Android. Unified Papaya Input layer:
Xbox, PlayStation, Nintendo, generic HID, Steam Input, touch.

### 101-104. Platforms
Steam Deck backend (SteamOS, Gamescope, Steam Input, HDR, VRR, controller,
performance profiles, TDP, GPU selection). Android backend (ARM64, Vulkan,
AAudio, Android Gamepad, Android filesystem, JNI, surface lifecycle, touch,
thermal, battery). Snapdragon optimization (Adreno Vulkan, ARM64 JIT tuning,
thermal-aware scheduling, memory bandwidth, shader cache, battery profiles;
targets: Snapdragon 8 Gen 2/3, Odin 2, Retroid). Raspberry Pi (BCM2712,
ARM64, Vulkan, Mesa, VC4/V3D).

### 105-107. Hardware detection / capability DB / benchmarks
Auto-detect CPU, GPU, RAM, VRAM, driver, Vulkan version, GL version,
Wayland, X11, PipeWire, kernel, filesystem. GPU → Vulkan capabilities →
D3D feature level → Papaya capability set. Own benchmark suite: PE loading,
DLL loading, syscalls, memory, threads, sync, filesystem, network, JIT,
D3D, shader compilation, present, audio, input. Application benchmark: FPS,
1% low, 0.1% low, frametime, CPU/GPU overhead, RAM/VRAM, shader stutter, JIT
stutter, loading time.

### 108-110. Testing infra
Commit-level regression testing of previously working applications. Windows
reference testing: real Windows behavior as reference (error codes etc.).
API conformance suite organized per DLL (ntdll, kernel32, kernelbase,
user32, gdi32, advapi32, ole32, oleaut32, shell32, ws2_32, winhttp,
wininet, bcrypt, crypt32, d3d9/10/11/12, dxgi, xaudio2, xinput).

### 111-112. Fuzzing
Every parser: PE, ICO, BMP, PNG, manifest, registry, MSI, resources, shader
bytecode (DXBC, DXIL), network packets, Windows paths, command lines.
libFuzzer, AFL++, Honggfuzz.

### 113-117. Security / resource control / telemetry / diagnosis / workarounds
Papaya Sandbox: filesystem isolation, network policy, device access, GPU
access, IPC restrictions — seccomp, Landlock, namespaces, cgroups; optional
per-game profiles. Resource control per app: CPU, RAM, GPU policy, disk
quota, network policy, process limits. Telemetry opt-in only. Crash →
analyze → identify subsystem → search compat DB → compare regressions →
recommend fix. Workaround generation: API failure → compat engine → known
mismatch → generate config → test → persist only if successful. No
arbitrary AI-generated runtime modifications without validation.

### 118-121. AI-assisted engineering / trace / replay / determinism
Dev-mode: trace, stack, API calls, GPU calls, crash dump → missing API,
Windows behavior, Papaya behavior, suggested implementation. papaya-trace
event types: PROCESS, THREAD, DLL, FILE, REGISTRY, MEMORY, SYNC, COM,
NETWORK, D3D, VULKAN, AUDIO, INPUT, EXCEPTION. Replay engine for
deterministic debugging. Determinism tools for race/timing/graphics/JIT bugs.

### 122. Logging architecture
No stdout spam. Structured logs: ERROR, WARN, INFO, DEBUG, TRACE, PERF, API,
GPU, JIT; JSON + human-readable + binary trace.

### 123-125. CLI / doctor / GUI
papaya run/install/inspect/diagnose/benchmark/trace/debug/env/cache/doctor/
update. papaya doctor checks Vulkan, GPU, drivers, Wayland/X11, PipeWire,
kernel, permissions, Android environment, Steam, filesystem, CPU features,
memory. Papaya Launcher: games, applications, compatibility, performance,
graphics, input, audio, storage, logs, diagnostics.

### 126-128. Install / dependency manager / .NET
Drag EXE here → inspect → identify → dependencies → configure → create env →
launch. Detect VC++ runtimes, .NET, DirectX components, WebView2, Media
Foundation, OpenAL, PhysX, XAudio — provide legitimate install mechanisms or
compatible runtime implementations. .NET strategy: native/open runtimes where
practical (do not recreate the CLR from scratch).

### 129-133. WinRT / UWP / MSIX / WebView2 / Electron
WinRT metadata, IInspectable, async operations, XAML. UWP/AppX/MSIX package
parsing, installation, manifests, identity, filesystem virtualization,
registration. WebView2 compatibility strategy. Chromium/Electron applications
(Discord, Slack, VS Code). Browser testing: Chrome, Edge, Firefox.

### 134-137. Non-gaming targets
Office (Word/Excel/PowerPoint/Outlook). Creative (Adobe, Autodesk, Blender
Windows build, DAWs, video editors, 3D applications). Dev tools (VS, MSVC,
Windows SDK, CMake Windows, Python/Node/Rust Windows binaries). Engine
matrix: Unity, Unreal, GameMaker, Godot, Source, RE Engine, Frostbite,
Decima.

### 138-140. Matrices
Anti-regression game matrix: game, version, engine, graphics API, anti-cheat,
launcher, DRM, known issues, FPS, status. Proton feature parity matrix
(Proton = feature benchmark, not implementation dependency): Wine runtime →
Papaya NT/Win32; DXVK → Papaya D3D9/10/11; VKD3D-Proton → Papaya D3D12;
WineD3D → Papaya fallback renderer; Wine sync → Papaya sync; Wine prefix →
Papaya environment; Wineserver → Papaya object/runtime manager; lsteamclient
→ Papaya Steam ABI; Wine Vulkan → Papaya Vulkan layer; Wine audio → Papaya
audio runtime; Wine input → Papaya input runtime; Wine networking → Papaya
networking.

### 141-144. Performance / memory / startup
Measurable: <5% CPU overhead typical, <10% worst-case, x86-64; <5% graphics
translation overhead where practical; frametime stability over average FPS.
Memory: shared immutable runtime, copy-on-write environments, shared shader
+ translation caches. Startup: EXE → first instruction in milliseconds;
lazy DLL loading, parallel dependency resolution, persistent caches,
precomputed API maps. Hot paths: syscalls, handles, locks, memory, DLL
resolution, graphics, JIT — never optimize on intuition.

### 145-147. ABIs / plugins / platform abstraction
Papaya Runtime ABI, Graphics ABI, Translation ABI, Plugin ABI. Plugin
system: GPU backend, audio backend, input backend, translation backend,
filesystem backend, platform backend, diagnostic backend. Papaya Core →
Linux, SteamOS, Android, future platforms.

### 148-152. Build / CI
CMake + Ninja. CI: cross compilation, sanitizers, LTO, PGO, release/debug,
ASAN/UBSAN/TSAN, coverage. Compiler matrix: Clang, GCC, Android NDK, ARM64,
x86-64. CI matrix: Ubuntu x86-64, Arch x86-64, SteamOS, Ubuntu ARM64,
Android ARM64, Raspberry Pi ARM64. Graphics CI: Mesa AMD/Intel/NVIDIA,
Qualcomm/Adreno where available, software Vulkan. ABI tests: x86, x86-64,
ARM64, ARM64EC.

### 153-156. Docs / SDK / reporting / capability negotiation
docs/: architecture, runtime, nt, win32, graphics, audio, input,
networking, jit, arm64, android, steam, compatibility, debugging, security,
contributing. Developer SDK: Papaya Runtime API. Compatibility reporting:
per-domain scores (e.g. Overall 87%, PE 100%, Win32 93%, COM 81%, D3D11 96%,
D3D12 72%, Audio 100%, Input 94%, Networking 100%). Capability negotiation:
host capabilities + runtime capabilities + application requirements → best
compatible configuration.

### 157-160. Version / registry / locale / virtual hardware
Configurable Windows 7/8.1/10/11 version behavior — emulate observable APIs/
behavior needed by apps, don't just lie. Registry compatibility profiles per
Windows version. Locale profiles: language, region, timezone, currency, code
page, keyboard. Virtual hardware: coherent CPU/GPU/RAM/display/audio/network/
storage rather than random spoofing.

### 161-163. GPU spoofing / patches / profiles
Only use GPU spoofing when required for compatibility; capability-based
compatibility instead of random names. Game-specific patches isolated:
compatibility/game/version/patch/ — never contaminate the core.
Compatibility profiles example:
```
application: example.exe
version: "1.2"
requirements:
  graphics: d3d12
  audio: xaudio2
  input: xinput
workarounds:
  - id: dxgi_present_mode
    condition: driver_bug
```

### 164-166. Regression / golden / differential testing
Commit X → game regression → bisect → identify subsystem → produce test.
Known-good outputs for API calls, filesystem, registry, graphics, timing,
exceptions. Differential testing: run Windows and Papaya with identical test
programs; compare return values, errors, structures, timing ranges, side
effects.

### 167-169. Coverage dashboard / stub lifecycle / priority
implemented, tested, passing, partial, stubbed, missing per DLL/API. A stub
needs TODO, owner, test, priority, known applications — and must eventually
disappear. Prioritize by application frequency, game frequency, dependency
depth, performance impact, implementation difficulty — not alphabetically.
Top-1,000-APIs project: automated ranking by real-world usage; top-100
applications; top-20 engines.

### 170-174. Conformance / timing / timers / APC / fibers
Graphics conformance: thousands of D3D call/shader/resource/swapchain tests.
Timing conformance: QPC, sleep, wait, timer resolution, scheduler behavior.
Timer subsystem: CreateTimerQueue, waitable timers, high-res timers,
multimedia timers, QPC, Sleep, SleepEx. APC: alertable waits, QueueUserAPC,
NtQueueApcThread. Fibers: CreateFiber, SwitchToFiber,
ConvertThreadToFiber, FLS.

### 175-176. Context switching / WOW64
GetThreadContext, SetThreadContext, Wow64 context, ARM64 context, x64
context, x86 context. WOW64-like: 32-bit app → Papaya WOW layer → 64-bit
Papaya runtime; 32-bit pointers/structures/WOW64 syscalls/32-bit DLLs/
32-bit TEB/PEB.

### 177-182. x86-on-ARM / ARM64EC / coherency
x86 Windows app → 32-bit Windows ABI → x86 translation → ARM64. ARM64EC:
ARM64 native DLL + x64 Windows code in one process. JIT memory coherency:
self-modifying code, instruction cache, page permissions, code invalidation,
thread synchronization.

### 183-185. Signals / syscall abstraction / Host ABI
SIGSEGV → ACCESS_VIOLATION, SIGILL → ILLEGAL_INSTRUCTION, SIGFPE →
arithmetic exception, SIGTRAP → breakpoint. Win32 → Papaya Runtime → Host
ABI → Linux/Android. Host ABI: memory, threads, files, network, timers,
synchronization, GPU, audio, input — makes Android support dramatically
easier.

### 186-191. Android integration / thermal / pacing / caches
JNI isolated at the boundary. ANativeWindowSurface, Vulkan surface,
lifecycle, orientation, fullscreen, touch, gamepad. Thermal: temperature,
CPU/GPU frequency, battery, power state → performance/balanced/battery
profiles. Frame pacing on handhelds: stable 30/40/45/60 FPS. Battery-aware
shader compilation (on charger or idle). Background cache building (JIT
blocks, shaders, pipelines after install).

### 192-200. Distribution / versioning / updates / offline / reproducible
AppImage, Flatpak, native Linux package, Steam compatibility tool, Android
package. Steam compatibility database → Papaya 0.x → 1.x with runtime +
compatibility DB versioning. 1.0 = stable runtime/environment/config/
diagnostics ABIs. Update system: runtime, compat DB, shader cache, JIT cache
invalidation independently. Offline mode; network-independent core. Locked
dependencies, reproducible toolchains, signed releases, SBOM, source
verification.

### 201-205. Security / parsing / archives / ISO
Treat every component (PE, DLL, shader, network, filesystem) as hostile
input. Sandboxed parsing for PE, MSI, shaders, archives. ZIP/7z/RAR/TAR/
ISO/CHD/SO with zip-bomb, path-traversal, symlink and resource-limit
protection. Safe mount/extraction abstractions.

### 206-212. Game/launcher detection / child processes / injection / DLLs
Identify Steam, GOG, Epic, EA, Ubisoft, Battle.net, standalone. Auto-detect
launcher.exe/game.exe/setup.exe. Multi-executable apps (launcher, helper,
game, anti-cheat, crash reporter) in one environment. Child process
inheritance: propagate runtime environment, handles, registry, filesystem,
compat profile, graphics context. Process-injection compat:
CreateRemoteThread, VirtualAllocEx, WriteProcessMemory, ReadProcessMemory,
OpenProcess with correct semantics inside Papaya's security model. Shared
DLL state; native DLL loading rules; DLL classification (Papaya impl /
Windows app DLL / system-like / optional dep / graphics runtime); DLL
conflict resolver (deterministic rules for game-shipped d3d11.dll etc.).

### 213-219. SxS / manifests / UAC / virtualization / locking / case
WinSxS-like assemblies, activation contexts, assembly identity, DLL
redirection. Parse app + assembly manifests: requestedExecutionLevel,
supportedOS, DPI awareness, dependencies, COM registration. UAC compat
semantics without pretending the host is Windows. Program Files write
virtualization + registry virtualization. Windows sharing semantics deserve
dedicated testing. Case-insensitive + case-preserving paths even on
case-sensitive filesystems.

### 220-226. NT namespace / named objects / notifications / console
\Device\DosDevices, \BaseNamedObjects. Global\ and Local\ namespaces. Named
shared memory + pipes. RegNotifyChangeKeyValue. ReadDirectoryChangesW via
inotify/fanotify. Windows console: ConPTY, console input/output, VT
behavior, UTF-16, code pages, console events.

### 227-233. PTY / headless / servers / networking perf / DNS / HTTP
ConPTY → PTY abstraction. GUI-less and headless modes for servers,
automation, CI. Test game servers, web servers, database tools. Near-native
latency/throughput/socket creation/IOCP. GetAddrInfo, DnsQuery. WinHTTP,
WinINet, URLMon with appropriate native backends.

### 234-240. Crypto / certs / DPAPI / credentials / identity
Root/My/CA/TrustedPeople stores. BCrypt, CryptAcquireContext,
CryptProtectData. DPAPI with Papaya environment-scoped secure storage.
Credential Manager with safe Linux-backed storage. Stable per-environment
identity: computer name, user name, domain, machine GUID (stable across
launches).

### 241-250. Registry boot / boot-like init / COM activation
Initialize SYSTEM, SOFTWARE, SECURITY, SAM, DEFAULT, USER, CLASSES as
required. Deterministic init: runtime → registry → services → COM →
graphics → audio → input → application. Service dependency graph; lazy
services. COM activation DB: CLSID, ProgID, AppID, TypeLib, Interface.
DllGetClassObject, DllCanUnloadNow. COM EXE servers (out-of-process).
Automation: IDispatch, OLE Automation, VARIANT, BSTR, SAFEARRAY.

### 251-253. Graphics interop / shared resources / video decode
GDI, D2D, D3D, DXGI, Media Foundation interop. Shared textures, shared
handles, fences, cross-API resources. Hardware decode: H.264, H.265, AV1,
VP9 through host APIs.

### 254-257. Rumble / motion / overlay
XInput vibration, DirectInput force feedback, haptics, adaptive triggers.
Gyro + accelerometer for handhelds. Overlay: FPS, frametime, CPU, GPU,
temperature, battery, JIT, shader compilation; hotkey-configurable.

### 258-264. Performance profiles / dynamic perf / optimization / Deck
Quality/Balanced/Performance/Battery/Custom. Auto-adjust resolution,
upscaling, frame caps, shader compilation, CPU affinity — explicit user
control only. Compat DB recommendations without modifying game files.
Steam Deck: Gamescope, Steam Input, SteamOS APIs. Controller-first UI for
Odin/Retroid/Steam Deck. Android launcher: games, recent, favorites,
performance, settings. Automatic cover/art metadata (optional network,
must work offline). Game database: title, executable, version, engine,
compatibility, performance, controller, graphics.

### 265-272. Scores / evidence / reports / bisector
Compatibility score: native/excellent/good/playable/broken/unsupported (not
arbitrary percentages alone). Evidence links: test result, runtime version,
hardware, game version. Community reports, privacy-respecting. Automatic bug
report: runtime version, hardware, logs, trace, crash dump, compat profile —
no private files. Reproduction packages. Compatibility bisector: version A
works / version B fails → identify regression. Runtime feature flags per
major subsystem.

### 273-280. Fallbacks / device loss / recovery / suspend
D3D12 unavailable → D3D11 fallback where the app supports it. Don't expose
unsupported features. Shader failure: diagnose → retry → fallback → report.
Device loss: DXGI_ERROR_DEVICE_REMOVED/RESET mapped from Vulkan device loss.
Graphics recovery: recreate device + swapchain + restore resources. Audio
recovery: device disconnected/changed, PipeWire restart. Network recovery:
interface changes, DNS changes, sleep/wake. Suspend/resume for handhelds:
sleep, resume, screen off, GPU reset, network change. Android lifecycle:
pause/resume, surface destroyed/recreated. Steam Deck suspend.

### 281-288. Save safety / crash-safe env / caches / self-test
Atomic writes, journaling, backup, rollback, conflict resolution. Registry
+ config transactional where practical. Atomic install with rollback. Cache
corruption recovery: JIT, shader, pipeline, metadata rebuild. Runtime
self-test at startup: Vulkan, audio, input, filesystem, JIT, quick checks.
papaya doctor --deep.

### 289-300. Dev mode / tracing / testing / env CLIs / release architecture
papaya dev: API calls, DLL loads, JIT, GPU, threads, handles, memory.
papaya trace --api/--gpu/--jit game.exe. papaya test/benchmark/regression
app.exe. papaya env create/delete/reset/backup. papaya cache
list/clear/rebuild. papaya package install/remove/verify. Release
architecture:
```
Papaya
├── Core Runtime (PE, NT, Win32, COM, Registry, FS, Security, IPC, Services)
├── Execution (x86, x64, ARM64, WOW64, ARM64EC, JIT)
├── Graphics (D3D9/10/11/12, DXGI, D2D, DWrite, Shader compiler, Vulkan)
├── Multimedia (XAudio2, WASAPI, DirectSound, Media Foundation, DirectShow)
├── Platform (Linux, SteamOS, Android, ARM64)
├── Integration (Steam, Steam Input, OpenXR, launchers)
├── Compatibility Intelligence (DB, profiler, diagnostics, regression, workarounds)
├── Performance (JIT, shader cache, pipeline cache, profiling, frame pacing)
├── Security (sandbox, seccomp, Landlock, namespaces, resource limits)
└── UX (launcher, CLI, overlay, controller UI, diagnostics)
```

## Priority note

Do not interpret this as "write 300 features and Papaya is done." The real
project is organized into dependency layers:

```
PHASE 0  Architecture cleanup
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
PHASE 14 Compatibility laboratory
PHASE 15 Massive application/game matrix
PHASE 16 Performance optimization
PHASE 17 Security hardening
PHASE 18 Papaya 1.0
```

And the single most important strategic difference from Proton: **Don't make
Papaya a collection of hacks that happens to launch games. Build a coherent
Windows execution environment first.** Wine's architecture is fundamentally
a Windows program loader plus a large implementation of Windows APIs over
Unix primitives; Proton adds the gaming stack. Papaya's opportunity is to
build that coherent runtime from scratch, optimized for modern
Linux/ARM64/Vulkan/Steam handhelds.

## Working definitions (from the companion spec, same intent)

- Behavioral compatibility matters: explicit error codes, timing,
  synchronization, threading, memory ordering, handle lifetime, filesystem
  behavior, DLL search order, registry behavior, Unicode, FP behavior,
  graphics state, shader behavior, audio timing, networking, process
  creation, exception handling.
- Practical objective: Windows ABI + Linux kernel compatibility:
```
                KLEIDI
        ┌───────────┴───────────┐
        │                       │
  Windows ABI             Linux Kernel compat
   ├── Win32                 ├── io_uring
   ├── COM                   ├── Vulkan
   ├── D3D                   ├── PipeWire
   ├── Winsock               ├── Wayland
   ├── WASAPI                ├── POSIX
   ├── Registry              └── Linux FS
   └── PE/COFF
```
- Userspace compatibility boundary: Windows userspace → Papaya userspace
  compatibility → Linux kernel; kernel-dependent software → compatibility
  strategy → Linux-native equivalent (never recreate ntoskrnl wholesale).
- Stub discipline: a stub should have a TODO, owner, test, priority, known
  applications, and eventually disappear. Never leave fake stubs indefinitely.
- API coverage dashboard tracks implemented/tested/passing/partial/stubbed/
  missing per DLL/API.

## Repository mapping (as of 2026-08-30)

- Loader/execution: `src/win32/src/pe_loader.cpp` (PE32/PE32+, imports,
  exports, relocations, TLS dir, .pdata registration, Heaven's Gate 32-bit
  compat mode), `src/win32/src/win32_seh.cpp` (signal→SEH dispatch,
  __C_specific_handler scope tables).
- Win32 HLE: `src/win32/src/win32_api_hle.cpp` (large; GDI, USER32, registry,
  winsock, winmm, msvcrt, kernel32, D3D11/DXGI software surface,
  DirectSound, DInput, XInput, Steam via `src/steam/`).
- Window manager (X11): `src/win32/src/win32_window.cpp` (+ hpp).
- Audio: `src/win32/src/win32_audio.cpp` (winmm + PulseAudio),
  `win32_dsound.cpp`.
- Frontend/orchestrator: `src/frontend/src/emulator_runtime.cpp`.
- Coverage: `docs/GAME_API_COVERAGE.md` (37 DLLs, 12,695 exports, ~2%).
- Capability status (honest): proof-of-concept HLE shell; boots small PE
  binaries; d3d12 0%, d3d9 0%, ddraw 0%, dsound 0%, d3d11 ~4%, dxgi ~5%;
  no functional GPU-D3D translation; far from Wine/Proton parity (orders of
  magnitude; multi-year effort).