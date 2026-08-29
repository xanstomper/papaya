#!/usr/bin/env python3
"""
Papaya Game Compatibility Database Client ("The Brain")
Queries community-sourced per-game configuration overrides, GPU spoofing directives,
and rendering parameters based on Steam AppID or ROM disc serial.
"""

import os
import json
from pathlib import Path
from typing import Dict, Any, Optional

DB_PATH = Path(__file__).parent.parent / "data" / "compatibility_db.json"

class CompatibilityDatabase:
    def __init__(self, db_file: Optional[Path] = None):
        self.db_file = db_file or DB_PATH
        self.db: Dict[str, Any] = {}
        self.load()

    def load(self):
        if self.db_file.exists():
            try:
                with open(self.db_file, "r", encoding="utf-8") as f:
                    self.db = json.load(f)
            except Exception as e:
                print(f"[WARN] Failed to load compatibility DB: {e}")
                self.db = {}
        else:
            self.db = {}

    def get_default_profile(self) -> Dict[str, Any]:
        return self.db.get("default_profile", {
            "strip_textures": False,
            "mip_lod_bias": 2.0,
            "force_fsr_upscaling": True,
            "internal_width": 960,
            "internal_height": 540,
            "fps_limit": 60,
            "anisotropy_clamp": 2,
            "disable_raytracing": True,
            "dxvk_state_cache": True,
            "dxvk_async_pipe": True,
            "dxvk_overrides": {
                "dxvk.enableAsync": "true",
                "dxvk.gpl": "true",
                "dxvk.allowMemoryOvercommit": "true"
            }
        })

    def query_profile(self, identifier: str) -> Dict[str, Any]:
        """
        Query profile by AppID (e.g. '1245620') or ROM serial (e.g. 'SLUS-20062').
        Falls back to default profile with any specific overrides merged.
        """
        profile = dict(self.get_default_profile())
        titles = self.db.get("titles", {})
        
        # Check direct key
        match = titles.get(str(identifier))
        if not match:
            # Check case-insensitive
            for k, v in titles.items():
                if k.lower() == str(identifier).lower():
                    match = v
                    break

        if match:
            profile.update(match)
            print(f"[BRAIN] Loaded specific profile for '{identifier}': {match.get('name', 'Custom Title')}")
        else:
            print(f"[BRAIN] No custom profile found for '{identifier}'; using adaptive default profile")

        return profile
