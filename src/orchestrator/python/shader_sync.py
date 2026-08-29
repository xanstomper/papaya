#!/usr/bin/env python3
"""
Papaya Decentralized Shader Sync Client
Pre-caches and synchronizes .dxvk-cache state files to eliminate first-time
Vulkan pipeline compilation stutter on low-power handhelds and SBCs.
"""

import os
import shutil
from pathlib import Path
from typing import Optional

DEFAULT_CACHE_DIR = Path.home() / "Papaya" / "ShaderCaches"

class ShaderCacheSync:
    def __init__(self, cache_dir: Optional[Path] = None):
        self.cache_dir = cache_dir or DEFAULT_CACHE_DIR
        self.cache_dir.mkdir(parents=True, exist_ok=True)

    def get_cached_shader_path(self, app_id_or_title: str) -> Path:
        return self.cache_dir / f"{app_id_or_title}.dxvk-cache"

    def inject_shader_cache(self, app_id_or_title: str, target_game_dir: Path) -> bool:
        """
        Copies pre-compiled community .dxvk-cache into game directory before launch.
        """
        source_cache = self.get_cached_shader_path(app_id_or_title)
        if source_cache.exists():
            target_cache = target_game_dir / f"{app_id_or_title}.dxvk-cache"
            try:
                shutil.copy2(source_cache, target_cache)
                print(f"[SHADER_SYNC] Injected pre-compiled state cache: {target_cache.name} ({source_cache.stat().st_size} bytes)")
                return True
            except Exception as e:
                print(f"[WARN] Failed to inject shader cache: {e}")
        else:
            # Create initialized zero-stutter placeholder cache
            target_cache = target_game_dir / f"{app_id_or_title}.dxvk-cache"
            if not target_cache.exists():
                with open(target_cache, "wb") as f:
                    # Write DXVK State Cache Header
                    f.write(b"DXVK\x01\x00\x00\x00")
                print(f"[SHADER_SYNC] Initialized fresh DXVK state cache for '{app_id_or_title}'")
                return True
        return False

    def export_shader_cache(self, app_id_or_title: str, game_dir: Path):
        """
        Collects newly generated cache after play session to sync back to repository.
        """
        game_cache = game_dir / f"{app_id_or_title}.dxvk-cache"
        if game_cache.exists() and game_cache.stat().st_size > 8:
            dest = self.get_cached_shader_path(app_id_or_title)
            shutil.copy2(game_cache, dest)
            print(f"[SHADER_SYNC] Exported runtime shader state cache ({game_cache.stat().st_size} bytes) -> {dest.name}")
