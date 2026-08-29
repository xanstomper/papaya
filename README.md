# Project Papaya (`papaya-emu`)

Next-Generation Xbox One & Xbox Series S/X Emulator targeting **Linux-first**, with future support for Windows (WHVP) and Android Handhelds (ARM64 JIT).

---

## Architecture Overview

```
+-------------------------------------------------------------------------+
|                              Title OS / GDK                             |
+-------------------------------------------------------------------------+
       |                        |                       |
       v                        v                       v
+------------------+  +-------------------+  +--------------------+
|   papaya-hle     |  |    papaya-gpu     |  |    papaya-audio    |
| (Kernel/Syscalls)|  | (AMD GCN -> Vulkan|  |  (SHAPE DSP ->     |
|                  |  |  1.3 / SPIR-V)    |  |   miniaudio/Cubeb) |
+------------------+  +-------------------+  +--------------------+
       |                        |                       |
+-------------------------------------------------------------------------+
|                               papaya-hv                                 |
|            Hypervisor Abstraction Layer (HAL) / Memory Manager          |
|    - Linux: KVM (/dev/kvm)                                              |
|    - Windows: WHVP                                                      |
|    - Android: ARM64 JIT / Dynamic Binary Translation (FEX-Emu / Box64)  |
+-------------------------------------------------------------------------+
                                |
+-------------------------------------------------------------------------+
|                             papaya-storage                              |
|                XVD / XVC Container Parser & Virtual File System         |
+-------------------------------------------------------------------------+
```

---

## Key Subsystems

| Module | Description |
| :--- | :--- |
| [`papaya-common`](file:///home/jewboy420/papaya/src/common) | Base types, error codes (`Result<T>`), memory utilities, logging |
| [`papaya-hv`](file:///home/jewboy420/papaya/src/hv) | Hardware virtualization engine (`/dev/kvm`), vCPU lifecycle, guest physical memory layout (8GB DDR3 + 32MB ESRAM / GDDR6 unified) |
| [`papaya-storage`](file:///home/jewboy420/papaya/src/storage) | XVD / XVC container parser, chunk indexing, decryption hooks, VFS mounting |
| [`papaya-hle`](file:///home/jewboy420/papaya/src/hle) | High-Level Emulation for Era OS / GDK syscalls, process context, hypercalls |
| [`papaya-gpu`](file:///home/jewboy420/papaya/src/gpu) | AMD GCN PM4 command processor, ring buffer parsing, SPIR-V translation, Vulkan 1.3 |
| [`papaya-audio`](file:///home/jewboy420/papaya/src/audio) | Scalable Hardware Audio Processing Engine (SHAPE) 256-voice DSP mixer |
| [`papaya-input`](file:///home/jewboy420/papaya/src/input) | Xbox controller state mapping, impulse triggers, vibration management |
| [`papaya-app`](file:///home/jewboy420/papaya/src/app) | Main CLI runner, subsystem coordinator, KVM diagnostics engine |

---

## Building and Running

### Prerequisites (Ubuntu / Debian)
```bash
sudo apt install build-essential cmake ninja-build libvulkan-dev libzstd-dev libsdl2-dev
```

Ensure `/dev/kvm` read/write access:
```bash
sudo usermod -aG kvm $USER
```

### Build
```bash
cd /home/jewboy420/papaya
./scripts/build.sh Release
```

### Run Tests & Diagnostics
```bash
./scripts/test.sh
```

### Run Emulator
```bash
# Run default self-test diagnostics & initialize all subsystems
./build/src/app/papaya

# Mount an XVD package and initialize
./build/src/app/papaya --mount-xvd /path/to/game.xvd --target xboxone
```
