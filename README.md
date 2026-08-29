# Project Papaya - ARM Steam & ROM Translation Layer

**Papaya** is a high-performance compatibility runtime, ROM translation layer, and optimization stack written in **C++23** designed to emulate and run **Windows Steam games, Disc Images (ISO 9660, UDF, CHD, CSO, BIN/CUE), and ROM packages on ARM Android handhelds (Snapdragon 8 Gen 2/3, AYN Odin 2, Retroid Pocket), single-board computers (Raspberry Pi 5 / BCM2712), and Linux devices (Steam Deck, Ubuntu, Arch)**.

---

## The 5-Layer Translation & Optimization Stack

```
                                  +---------------------------------------+
                                  |         Host OS / Target Device       |
                                  |   Linux ARM64 / Android / Linux x86   |
                                  +-------------------+-------------------+
                                                      |
                                                      v
+---------------------------------------------------------------------------------------------------------+
|                                    Papaya ARM Steam & ROM Translation Layer                             |
+---------------------------------------------------------------------------------------------------------+
|                                                                                                         |
|  1. ROM / Disc Image & Storage Subsystem (papaya-rom)                                                   |
|     * Universal ISO / CSO / CHD / BIN+CUE disc image parser and Logical Block Address (LBA) streamer    |
|     * ROM-to-Steam Bridge: Generates deterministic Virtual Steam AppIDs & Steam shortcuts for any ROM   |
|     * Android Storage Access Framework (SAF) & NDK JNI bridge (org.papaya.emulator.PapayaNativeBridge) |
|                                                                                                         |
|  2. Steamworks API Stub & DRM Bypass (papaya-steam)                                                     |
|     * Clean-room Goldberg-style Steam emulator stub bypassing heavy Chromium / CEF Steam client         |
|     * Emulates ISteamUser, ISteamFriends, ISteamUtils, ISteamUserStats, ISteamApps, ISteamInput         |
|     * Local achievement unlocking, offline stat persistence, and automatic AppID discovery              |
|                                                                                                         |
|  3. CPU Dynamic Recompiler & 16KB Page Bridge (papaya-cpu)                                              |
|     * Dynamic JIT Translation interface (Box64 / FEX-Emu) for x86/x64 to ARM64 instructions             |
|     * Android 15+ 16KB Kernel Page Size Compatibility Layer: Sub-page translation for 4KB PC binaries   |
|     * Direct native execution mode on x86-64 Linux host (Steam Deck / Desktop PC)                       |
|                                                                                                         |
|  4. Potato Mode Graphics Interceptor (papaya-gpu)                                                       |
|     * Mipmap LOD Bias Clamping: Intercepts sampler states, forcing +3.0/+4.0 LOD bias for tiny mips     |
|     * "Potato Mode" Texture Stripping: Replaces heavy 4K textures with 1x1 flat RGBA buffers in VRAM    |
|     * Heavy Post-Processing Shader Stripping: Detects and replaces compute/SSAO/fog with no-ops        |
|     * Swapchain Resolution Scaling & Spatial Upscaler: Forces 540p render targets upscaled to 1080p     |
|     * Asynchronous GPL Pipeline Compilation: Eliminates shader compilation stutter                      |
|                                                                                                         |
|  5. Hardware Profiler, Spoofing & Memory Watchdog (papaya-profile)                                      |
|     * GPU Hardware Spoofing: Reports Intel HD 4000 (1GB VRAM) to trigger lowest game preset fallbacks   |
|     * Auto-Configurator: Auto-detects Raspberry Pi 5, Snapdragon, Steam Deck, or Desktop PC             |
|     * Memory Watchdog: Monitors RAM pressure on unified mobile chips and flushes caches at 85% load     |
|                                                                                                         |
|  6. OS Bridge, NTSync & io_uring Direct I/O (papaya-kernel)                                            |
|     * NTSync Kernel Synchronization: Driver client (/dev/ntsync) eliminating user-space futex overhead  |
|     * io_uring Direct I/O: Asynchronous file read streaming bypassing POSIX blocking file APIs          |
|     * Wine Prefix & PRoot Container Sandbox: Manages Windows C:\ drive, AppData, and user paths         |
|                                                                                                         |
|  7. Audio & Input Subsystems (papaya-audio & papaya-input)                                              |
|     * Audio Bridge: Low-latency WASAPI / DirectSound translation to PulseAudio / AAudio                 |
|     * Virtual XInput Daemon: Maps Android touch & handheld buttons (AYN Odin 2) to Xbox 360 gamepads    |
|                                                                                                         |
+---------------------------------------------------------------------------------------------------------+
```

---

## Build and Test

### Requirements
- C++23 compliant compiler (`clang-18+` or `gcc-14+`)
- CMake 3.22+ & Ninja

### Compile
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Run Test Suites (15 / 15 Passing)
```bash
ctest --test-dir build --output-on-failure
```

### Launch Options

#### 1. Play a ROM/ISO through Steam Layer:
```bash
./build/src/app/papaya --rom /path/to/game.iso --potato --tier high
```

#### 2. Play a Windows Steam Game:
```bash
./build/src/app/papaya --game /path/to/game.exe --potato --tier low
```
