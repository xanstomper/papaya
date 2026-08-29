# Project Papaya Architecture Specification

## 1. Hardware Architecture Mapping

| Component | Target Console | Emulation Strategy |
| :--- | :--- | :--- |
| **CPU** | 8-core AMD Jaguar @ 1.75GHz (Xbox One) / Zen 2 (Series) | Linux Native KVM vCPUs (`/dev/kvm`), 1:1 hardware execution |
| **GPU** | AMD GCN 1.1 (12 CUs, 768 shaders) / RDNA 2 | Vulkan 1.3 backend, GCN PM4 command processor, SPIR-V translation |
| **System RAM** | 8GB DDR3 (Xbox One) / 10GB-16GB GDDR6 (Series) | Anonymous mmap backed by Transparent HugePages (THP) |
| **ESRAM** | 32MB Embedded SRAM @ 204 GB/s | Dedicated low-latency GPA mapping at `0x2_0000_0000` |
| **Audio** | SHAPE (Scalable Hardware Audio Processing Engine) | DSP voice mixer with 256 hardware channels |
| **Storage** | XVD / XVC encrypted packages | Header decryption, chunk streaming, VFS node abstraction |
| **Input** | Xbox One Wireless Controller | XInput button bitfields & 4-motor impulse rumble via SDL/evdev |

## 2. Directory Layout

```
papaya/
├── CMakeLists.txt              # Root build configuration
├── README.md                   # Quickstart guide
├── ARCHITECTURE.md             # Detailed technical spec
├── scripts/
│   ├── build.sh                # CMake & Ninja build script
│   └── test.sh                 # Test suite runner
├── src/
│   ├── common/                 # Base types, logging, Result<T>, memory utils
│   ├── hv/                     # KVM / WHVP / ARM-JIT hypervisor engine & memory map
│   ├── storage/                # XVD/XVC package parser and Virtual File System
│   ├── hle/                    # OS kernel runtime & syscall dispatch
│   ├── gpu/                    # AMD GCN PM4 processor & Vulkan 1.3 backend
│   ├── audio/                  # SHAPE audio DSP subsystem
│   ├── input/                  # Controller mapping and vibration
│   └── app/                    # Main CLI entry point & diagnostics harness
└── tests/
    ├── test_kvm_boot.cpp       # KVM guest payload execution test
    └── test_gpu_pm4.cpp        # GCN PM4 packet decoder unit test
```
