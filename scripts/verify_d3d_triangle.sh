#!/bin/bash
# End-to-end windowed D3D11 render verification (Stage 4h):
# run d3d_triangle under Xvfb with PAPAYA_VULKAN=1 and assert the FULL guest
# flow: Create*Shader with real DXBC -> bind pipeline state -> Present, with
# the papaya GPU pipeline path actually rendering (PAPAYA_GPU_PRESENT marker
# printed by sc_present after a successful render_and_present). Render
# correctness itself is pixel-verified by the offscreen readback test
# (test_vulkan_pipeline); WSI visibility on Xvfb is host-flaky (lavapipe xlib
# present / realtime-thread glibc issues), so pixels are reported, not
# asserted. Skips cleanly when Xvfb is missing.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
PAPAYA_BIN="${PAPAYA_BIN:-$REPO/build/src/app/papaya}"
GUEST="${1:-$REPO/build/guests/d3d_triangle.exe}"
DISP=":97"
OUT="/tmp/papaya_triangle.png"

command -v Xvfb >/dev/null || { echo "skip: Xvfb missing"; exit 0; }
[ -x "$PAPAYA_BIN" ] || { echo "error: papaya not built at $PAPAYA_BIN" >&2; exit 2; }
[ -f "$GUEST" ] || { echo "error: guest not built: $GUEST" >&2; exit 2; }

Xvfb "$DISP" -screen 0 320x240x24 >/dev/null 2>&1 &
XVFB_PID=$!
trap 'kill $XVFB_PID 2>/dev/null' EXIT
sleep 1

work="$(mktemp -d)"
cp "$GUEST" "$work/"
(
    cd "$work"
    DISPLAY="$DISP" PAPAYA_VULKAN=1 PAPAYA_D3D_TRACE=1 \
    VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}" \
    timeout 25 "$PAPAYA_BIN" d3d_triangle.exe >run.log 2>&1
)
RC=$?

if [ "$RC" != "0" ]; then
    echo "fail: guest exited $RC"
    tail -3 "$work/run.log" | sed 's/\x1b\[[0-9;]*m//g' | cut -c1-140
    exit 1
fi

grep -aq "d3dtri ok" "$work/run.log" || { echo "fail: guest did not reach present"; exit 1; }
if grep -aq "PAPAYA_GPU_PRESENT" "$work/run.log"; then
    echo "ok: d3d_triangle: full vtable flow + GPU pipeline present (translated DXBC->SPIR-V->VkPipeline)"
else
    echo "fail: GPU present marker missing (CPU fallback path only)"
    exit 1
fi
exit 0