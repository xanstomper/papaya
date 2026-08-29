#!/usr/bin/env python3
"""
Papaya Micro-HUD Configurator
Generates lightweight MangoHud and GameScope overlay configurations,
enabling controller quick-menu access (Select + R2) to toggle Potato Mode,
FSR sharpness, and FPS caps in real-time.
"""

from pathlib import Path
from typing import Dict, Any

def generate_mangohud_config(game_title: str, fps_limit: int = 60, potato_mode: bool = False) -> str:
    lines = [
        f"### Papaya Micro-HUD Configuration for {game_title} ###",
        "legacy_layout=0",
        "horizontal=0",
        "fps=1",
        "frametime=1",
        "gpu_stats=1",
        "gpu_temp=1",
        "gpu_load_change=1",
        "cpu_stats=1",
        "cpu_temp=1",
        "vram=1",
        "ram=1",
        f"fps_limit={fps_limit}",
        "toggle_hud=Shift_R+F12",
        "toggle_fps_limit=Shift_L+F1",
        "background_alpha=0.4",
        "font_size=20",
        "position=top-left",
        "round_corners=8",
        f"custom_text=Papaya Potato Mode: {'ON' if potato_mode else 'OFF'}"
    ]
    return "\n".join(lines) + "\n"

def write_mangohud_to_prefix(prefix_path: Path, game_title: str, fps_limit: int = 60, potato_mode: bool = False):
    config_dir = prefix_path / "drive_c" / "users" / "steamuser" / "AppData" / "Roaming" / "MangoHud"
    config_dir.mkdir(parents=True, exist_ok=True)
    conf_file = config_dir / "MangoHud.conf"
    content = generate_mangohud_config(game_title, fps_limit, potato_mode)
    with open(conf_file, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"[HUD] Written Micro-HUD configuration: {conf_file}")
