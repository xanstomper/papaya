#!/usr/bin/env python3
"""
Papaya Save Manager ("The Vault")
Monitors isolated game prefixes and automatically redirects/symlinks save folders
(AppData/Local, AppData/Roaming, Documents/My Games, Saved Games) to centralized host storage
~/Papaya/Saves/<GameIdentifier>/ to guarantee save files are preserved.
"""

import os
import shutil
from pathlib import Path
from typing import List, Optional

DEFAULT_VAULT_ROOT = Path.home() / "Papaya" / "Saves"

KNOWN_SAVE_SUBPATHS = [
    Path("users/steamuser/AppData/Local"),
    Path("users/steamuser/AppData/Roaming"),
    Path("users/steamuser/Documents/My Games"),
    Path("users/steamuser/Documents"),
    Path("users/steamuser/Saved Games"),
    Path("Program Files (x86)/Steam/userdata")
]

class SaveVaultManager:
    def __init__(self, vault_root: Optional[Path] = None):
        self.vault_root = vault_root or DEFAULT_VAULT_ROOT
        self.vault_root.mkdir(parents=True, exist_ok=True)

    def get_game_save_vault(self, game_id: str) -> Path:
        p = self.vault_root / str(game_id)
        p.mkdir(parents=True, exist_ok=True)
        return p

    def link_prefix_saves(self, prefix_path: Path, game_id: str, game_title: str):
        """
        Creates centralized save symlinks within the newly generated prefix.
        """
        drive_c = prefix_path / "drive_c"
        if not drive_c.exists():
            return

        game_vault = self.get_game_save_vault(game_id)
        
        # 1. Documents / My Games / <GameTitle>
        docs_my_games = drive_c / "users" / "steamuser" / "Documents" / "My Games" / game_title
        docs_my_games.parent.mkdir(parents=True, exist_ok=True)
        
        vault_docs = game_vault / "My Games"
        vault_docs.mkdir(parents=True, exist_ok=True)
        
        if not docs_my_games.exists() and not docs_my_games.is_symlink():
            try:
                docs_my_games.symlink_to(vault_docs, target_is_directory=True)
                print(f"[VAULT] Symlinked '{docs_my_games}' -> '{vault_docs}'")
            except Exception as e:
                print(f"[WARN] Failed to create symlink: {e}")

        # 2. AppData/Local / <GameTitle>
        appdata_local = drive_c / "users" / "steamuser" / "AppData" / "Local" / game_title
        appdata_local.parent.mkdir(parents=True, exist_ok=True)
        vault_appdata = game_vault / "AppData_Local"
        vault_appdata.mkdir(parents=True, exist_ok=True)

        if not appdata_local.exists() and not appdata_local.is_symlink():
            try:
                appdata_local.symlink_to(vault_appdata, target_is_directory=True)
                print(f"[VAULT] Symlinked '{appdata_local}' -> '{vault_appdata}'")
            except Exception as e:
                print(f"[WARN] Failed to create symlink: {e}")

    def create_save_snapshot(self, game_id: str, tag: str = "backup") -> Optional[Path]:
        game_vault = self.get_game_save_vault(game_id)
        archive_path = self.vault_root / f"{game_id}_{tag}.tar.gz"
        try:
            shutil.make_archive(str(self.vault_root / f"{game_id}_{tag}"), "gztar", str(game_vault))
            print(f"[VAULT] Created snapshot archive: {archive_path}")
            return archive_path
        except Exception as e:
            print(f"[WARN] Snapshot failed: {e}")
            return None
