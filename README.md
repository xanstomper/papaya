# Project Papaya - Autonomous ARM Steam & ROM Gaming Runtime

**Papaya** is a high-performance compatibility runtime, ROM translation layer, and autonomous orchestration daemon written in **C++23 & Python** designed to deliver a **zero-friction "drag-and-drop" console experience** on **ARM Android handhelds (Snapdragon 8 Gen 2/3, AYN Odin 2, Retroid Pocket 5), single-board computers (Raspberry Pi 5 / BCM2712), and Linux handhelds (Steam Deck, Ubuntu, Arch)**.

---

## The Complete Papaya Autonomous Architecture

```
                    +-------------------------------------------------------------+
                    |    User Drops File (ISO / ROM / ZIP / RAR / 7Z / EXE)      |
                    |             into ~/Papaya/Games/ Drop Folder                |
                    +------------------------------+------------------------------+
                                                   |
                                                   v
+---------------------------------------------------------------------------------------------------------+
|                                Papaya Orchestration Daemon Pipeline                                     |
+---------------------------------------------------------------------------------------------------------+
|                                                                                                         |
|  Phase 1: The Ingestion Engine (Auto-Unpack & Staging)                                                  |
|     * inotify file watcher detects incoming game files immediately                                      |
|     * Automatically unpacks multi-part ZIP/TAR/7Z archives and mounts ISO/CSO/CHD images                |
|                                                                                                         |
|  Phase 2: The Prefix Sandbox Builder                                                                    |
|     * Generates isolated Wine sandboxes: ~/Papaya/Prefixes/prefix_<GameID>/                             |
|     * Zero cross-game contamination: each title receives its own pristine Windows registry              |
|                                                                                                         |
|  Phase 3: The Brain (Game Compatibility Database) & Hardware Profiler                                   |
|     * Cross-references game against compatibility_db.json (Elden Ring, Cyberpunk 2077, GTA, BG3)      |
|     * Injects custom dxvk.conf: Adaptive LOD bias (+2.5), 1x1 flat textures, forced FSR 540p upscaling  |
|     * Hardware Spoofing: Injects registry keys spoofing low-end GPUs (Intel HD 4000) to force fallbacks |
|                                                                                                         |
|  Phase 4: DRM Stubbing (Goldberg & Offline Decoupling)                                                  |
|     * Locates and deletes official steam_api64.dll / steam_api.dll DRM libraries                        |
|     * Injects Goldberg Steamworks API Stub & generates steam_appid.txt for 100% offline gameplay        |
|                                                                                                         |
|  Phase 5: The Vault (Centralized Save Manager)                                                          |
|     * Symlinks in-prefix save folders (AppData/Local, AppData/Roaming, My Games) to host                |
|     * Preserves all save files safely at ~/Papaya/Saves/<GameID>/ even if prefixes are deleted          |
|                                                                                                         |
|  Phase 6: Decentralized Shader Sync                                                                     |
|     * Injects pre-compiled .dxvk-cache state files into prefix, eliminating first-time shader stutters |
|                                                                                                         |
|  Phase 7: Native UI & Micro-HUD Integration                                                             |
|     * Generates native Linux .desktop launchers and Steam Big Picture shortcuts                         |
|     * Configures MangoHud overlay with gamepad combo (Select + R2) to toggle Potato Mode on the fly     |
|                                                                                                         |
|  Phase 8: Papaya Net (P2P Multiplayer LAN Tunnel)                                                       |
|     * Translates Goldberg LAN broadcast packets into WireGuard P2P tunnels using 6-digit room codes     |
|                                                                                                         |
+---------------------------------------------------------------------------------------------------------+
```

---

## Build and Test

### Requirements
- C++23 compliant compiler (`clang-18+` or `gcc-14+`)
- CMake 3.22+ & Ninja
- Python 3.10+

### Compile
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Run All 17 Test Suites
```bash
ctest --test-dir build --output-on-failure
```

---

## Running the Autonomous Daemon

Start the background daemon to watch your games folder:
```bash
python3 src/orchestrator/python/papaya_daemon.py --watch ~/Papaya/Games
```

When you drag any ISO or game ZIP into `~/Papaya/Games/`, Papaya will automatically:
1. Ingest & extract the game.
2. Build an isolated Wine prefix sandbox.
3. Apply game-specific performance hacks from `compatibility_db.json`.
4. Decouple Steam DRM with the Goldberg stub.
5. Symlink saves to the Save Vault.
6. Generate a 1-click launcher in `~/Papaya/Shortcuts/`.
