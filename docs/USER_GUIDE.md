# Papaya User & Operator Guide

Welcome to **Project Papaya**, the autonomous compatibility runtime and ROM translation matrix designed for handhelds, SBCs, and Linux PCs.

---

## 1. Quick Start: The "Drag-and-Drop" Experience

1. Start the Papaya Orchestration Daemon:
   ```bash
   python3 src/orchestrator/python/papaya_daemon.py --watch ~/Papaya/Games
   ```
2. Drop any game file into `~/Papaya/Games/`:
   - Optical Disc Image (`.iso`, `.cso`, `.chd`, `.bin`, `.cue`)
   - Console ROM package (`.nsp`, `.xci`, `.rvz`)
   - Windows game archive (`.zip`, `.7z`, `.rar`, `.tar.gz`)
   - Direct game executable (`.exe`)
3. **Wait 10–20 seconds:**
   - Papaya automatically unpacks the archive or mounts the ISO.
   - Builds an isolated sandbox prefix at `~/Papaya/Prefixes/prefix_<GameID>/`.
   - Cross-references the title in `compatibility_db.json` and injects custom DXVK potato graphics hacks.
   - Replaces official Steam DRM with the Goldberg offline stub.
   - Links save folders to `~/Papaya/Saves/<GameID>/`.
   - Injects pre-compiled `.dxvk-cache` shader files to eliminate stutters.
   - Generates a native 1-click launcher in `~/Papaya/Shortcuts/`.

---

## 2. Interactive Terminal Dashboard (TUI)

Launch the real-time TUI monitor on your device:
```bash
python3 src/orchestrator/python/papaya_tui.py
```
Provides live updates on:
- System RAM pressure & memory watchdog status
- Ingested games and active sandboxed prefixes
- Save Vault status and backup snapshot health
- P2P Multiplayer tunnel status

---

## 3. In-Game Micro-HUD Controls (Select + R2)

When playing any title, press **Select + R2** on your controller to summon the transparent MangoHud Quick Menu:
- Toggle **Potato Mode** on/off without restarting the game.
- Adjust **FSR Sharpness** and rendering resolution scaling.
- Lock FPS at **30 FPS / 60 FPS**.

---

## 4. Save Vault Backup & Cloud Sync

All game saves are decoupled from the Wine prefixes and safely centralized at:
```
~/Papaya/Saves/<GameID>/
```
Even if you wipe a game's prefix or delete the game files, your saves and achievements are 100% preserved.
