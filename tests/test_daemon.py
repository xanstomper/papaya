#!/usr/bin/env python3
"""
Unit and integration test suite for the Papaya Python Orchestration Daemon.
"""

import os
import sys
import shutil
import zipfile
from pathlib import Path

# Add src/orchestrator/python to pythonpath
sys.path.insert(0, str(Path(__file__).parent.parent / "src" / "orchestrator" / "python"))

from compatibility_db import CompatibilityDatabase
from save_vault import SaveVaultManager
from shader_sync import ShaderCacheSync
from papaya_net import PapayaNetRoom
from hud_config import generate_mangohud_config, write_mangohud_to_prefix
from papaya_daemon import PapayaOrchestrator

def test_compatibility_database():
    print("[TEST] Running test_compatibility_database...")
    db = CompatibilityDatabase()
    elden_profile = db.query_profile("1245620")
    assert elden_profile["name"] == "Elden Ring"
    assert elden_profile["fps_limit"] == 30
    assert elden_profile["strip_textures"] is False

    cyberpunk_profile = db.query_profile("1091500")
    assert cyberpunk_profile["name"] == "Cyberpunk 2077"
    assert cyberpunk_profile["strip_textures"] is True
    assert cyberpunk_profile["mip_lod_bias"] == 2.5
    print("[TEST] test_compatibility_database PASSED!")

def test_save_vault_and_shaders():
    print("[TEST] Running test_save_vault_and_shaders...")
    test_dir = Path("./tmp_test_vault")
    test_dir.mkdir(parents=True, exist_ok=True)

    vault = SaveVaultManager(test_dir / "Saves")
    game_vault = vault.get_game_save_vault("1245620")
    assert game_vault.exists()

    shaders = ShaderCacheSync(test_dir / "Shaders")
    game_dir = test_dir / "GameDir"
    game_dir.mkdir(parents=True, exist_ok=True)
    injected = shaders.inject_shader_cache("1245620", game_dir)
    assert injected is True
    assert (game_dir / "1245620.dxvk-cache").exists()

    shutil.rmtree(test_dir, ignore_errors=True)
    print("[TEST] test_save_vault_and_shaders PASSED!")

def test_papaya_net():
    print("[TEST] Running test_papaya_net...")
    room = PapayaNetRoom("123456")
    assert room.subnet_ip.startswith("10.99.")
    assert room.connect() is True
    cfg = room.generate_goldberg_broadcast_config()
    assert len(cfg["custom_broadcasts"]) > 0
    room.disconnect()
    print("[TEST] test_papaya_net PASSED!")

def test_orchestrator_pipeline():
    print("[TEST] Running test_orchestrator_pipeline...")
    sandbox = Path("./tmp_orchestrator_sandbox")
    shutil.rmtree(sandbox, ignore_errors=True)
    sandbox.mkdir(parents=True, exist_ok=True)

    games_dir = sandbox / "Games"
    games_dir.mkdir(parents=True, exist_ok=True)

    # Create a mock zip game archive
    mock_zip_file = games_dir / "Cyberpunk_Handheld.zip"
    with zipfile.ZipFile(mock_zip_file, "w") as zf:
        zf.writestr("Cyberpunk.exe", b"MZ\x90\x00MockCyberpunkBinary")
        zf.writestr("steam_api64.dll", b"MZ\x90\x00OfficialSteamDRMDll")
        zf.writestr("steam_appid.txt", "1091500\n")

    orchestrator = PapayaOrchestrator(watch_dir=games_dir, papaya_home=sandbox)
    orchestrator.run_poll_loop(single_pass=True)

    # Verify pipeline results
    prefix_dir = sandbox / "Prefixes" / "prefix_1091500"
    staging_dir = sandbox / "Staging" / "Cyberpunk_Handheld"
    shortcuts_dir = sandbox / "Shortcuts"

    assert prefix_dir.exists(), "Prefix directory must exist"
    assert (staging_dir / "dxvk.conf").exists(), "dxvk.conf must be generated"
    assert (staging_dir / "steam_appid.txt").exists(), "steam_appid.txt must be generated"
    assert (staging_dir / "steam_api64.dll").exists(), "steam_api64.dll must be stubbed"
    assert (shortcuts_dir / "papaya_1091500.desktop").exists(), "Desktop shortcut must exist"

    # Verify dxvk.conf contents for Cyberpunk
    dxvk_text = (staging_dir / "dxvk.conf").read_text(encoding="utf-8")
    assert "papaya.potatoMode = true" in dxvk_text
    assert "papaya.mipLodBias = 2.5" in dxvk_text

    shutil.rmtree(sandbox, ignore_errors=True)
    print("[TEST] test_orchestrator_pipeline PASSED ALL CHECKS!")

if __name__ == "__main__":
    test_compatibility_database()
    test_save_vault_and_shaders()
    test_papaya_net()
    test_orchestrator_pipeline()
    print("\n>>> ALL PYTHON ORCHESTRATOR TESTS PASSED! <<<\n")
