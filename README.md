# Project Papaya (papaya-ps)

**Papaya** is an open-source, high-performance, backwards-compatible **PlayStation 4 (Orbis OS)** and **PlayStation 5 (Prospero OS)** emulator and compatibility layer written in modern **C++23**.

---

## Architecture Overview

```
                          +------------------------------------+
                          |       Host OS (Linux x86-64)       |
                          |   /dev/kvm  *  Vulkan  *  Audio    |
                          +------------------+-----------------+
                                             |
                                             v
+------------------------------------------------------------------------------------+
|                             Papaya PlayStation Core                                |
+------------------------------------------------------------------------------------+
|                                                                                    |
|  1. Binary Loader & Storage (papaya-storage)                                       |
|     * 64-bit ELF (eboot.bin) & .prx dynamic library parser                         |
|     * Automatic console detection: PS4 Orbis OS vs PS5 Prospero OS                |
|     * param.sfo & param.json package metadata parsers                              |
|     * PlayStation VFS mount hierarchy (/app0/, /savedata0/, /temp0/, /hostapp/)    |
|     * Oodle Kraken & LZ4 high-speed hardware asset decompressor                    |
|                                                                                    |
|  2. Kernel Translation Layer (papaya-hle)                                          |
|     * FreeBSD 9 & 12 syscall shims (sys_mmap, sys_thr_create, sys_umtx_op)         |
|     * Unified Memory Manager: 8 GB GDDR5 (PS4) & 16 GB GDDR6 (PS5)                |
|     * Direct Physical Memory Allocator: WB_ONION (coherent) & WC_GARLIC (GPU)     |
|     * Sony NID Database: instant O(1) hash resolution for libkernel, libScePad,    |
|       libSceAudioOut, libSceSaveData, libSceAgc, libSceGnmDriver, libSceFios2      |
|                                                                                    |
|  3. Video Core (papaya-gpu)                                                        |
|     * AMD PM4 Command Processor (PS4 GCN & PS5 RDNA 2 AGC command rings)           |
|     * Hardware Context State Machine (Render targets, blend modes, depth/stencil)  |
|     * GCN 1.1 / RDNA 2 ISA shader disassembler & SPIR-V 1.3 translation engine     |
|     * Vulkan 1.3 Dynamic Rendering backend with PSO disk caching                   |
|                                                                                    |
|  4. Audio Subsystem (papaya-audio)                                                 |
|     * libSceAudioOut 48kHz PCM port routing                                        |
|     * 3D Tempest Audio HRTF spatial software DSP mixer                             |
|                                                                                    |
|  5. Input Subsystem (papaya-input)                                                 |
|     * DualShock 4 & DualSense controller state mapping                             |
|     * Touchpad coordinates, 6-axis gyro/accelerometer, and adaptive trigger haptics|
|                                                                                    |
|  6. Execution & Frontend (papaya-frontend)                                         |
|     * x86-64 Host: Direct Native Execution & KVM 64-bit Long Mode hypervisor       |
|     * ARM64 Host: FEX-Emu / Box64 dynamic binary translation (Android handhelds)   |
|     * WindowManager & EmulatorRuntime orchestration loop                           |
|                                                                                    |
+------------------------------------------------------------------------------------+
```

---

## Building and Running

### Prerequisites
- Modern C++23 compiler (`clang-18+` or `gcc-14+`)
- CMake 3.22+ & Ninja
- Linux x86-64 with `/dev/kvm` access or Windows 11
- Vulkan 1.3 capable GPU and drivers

### Build
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Run Test Suite
```bash
ctest --test-dir build --output-on-failure
```

### Boot a Title
```bash
./build/src/app/papaya --target ps5 --boot /path/to/eboot.bin
```
