# Papaya Game Compatibility Database Guide

The **Game Compatibility Database ("The Brain")** (`src/orchestrator/data/compatibility_db.json`) dictates per-game optimizations applied automatically during ingestion.

---

## Adding a Custom Title

To add custom optimizations for a title, add an entry under the `titles` object in `compatibility_db.json`:

```json
"1245620": {
  "name": "Elden Ring",
  "strip_textures": false,
  "mip_lod_bias": 1.0,
  "force_fsr_upscaling": true,
  "internal_width": 960,
  "internal_height": 540,
  "fps_limit": 30,
  "anisotropy_clamp": 4,
  "disable_raytracing": true,
  "dxvk_overrides": {
    "dxvk.enableAsync": "true",
    "dxvk.gpl": "true"
  },
  "notes": "Force swapchain FSR 540p and cap at 30 FPS for optimal battery and frame pacing."
}
```

---

## Available Parameters

- `strip_textures` (`bool`): Enables 1x1 flat RGBA texture replacement for oversized textures.
- `mip_lod_bias` (`float`): Injects positive LOD bias into sampler states (`+2.0` to `+4.0`).
- `force_fsr_upscaling` (`bool`): Forces internal rendering at low resolution (e.g. 540p) with spatial upscaling.
- `internal_width` / `internal_height` (`int`): Internal render target dimensions.
- `fps_limit` (`int`): Caps frame presentation rate (30, 45, or 60).
- `anisotropy_clamp` (`int`): Limits anisotropic filtering to save memory bandwidth (2, 4, 8).
- `dxvk_overrides` (`dict`): Raw key-value overrides written to the prefix's `dxvk.conf`.
