#!/usr/bin/env bash
# Build the Papaya guest verification suite (tests/guests/*.c) with the
# real mingw-w64 cross-compiler into build/guests/.
# Usage: scripts/build_guests.sh [output_dir]
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$REPO/build/guests}"
CC="${CC:-x86_64-w64-mingw32-gcc}"
command -v "$CC" >/dev/null || { echo "error: $CC not found (apt install gcc-mingw-w64-x86-64)" >&2; exit 2; }
mkdir -p "$OUT"

build() { # name, extra libs...
    local name="$1"; shift
    echo "CC  $name.exe"
    "$CC" -O1 -Wall -Wno-format -o "$OUT/$name.exe" "$REPO/tests/guests/$name.c" "$@"
}

# CRT / locale / boot-noise suite (exit 0, zero unresolved imports expected)
build msvcrt_boot
# wsprintfA spec suite (register + stack varargs, size prefixes, flags)
build wstest      -luser32
build wsprintf_boot -luser32
# msvcrt FILE* registry + fopen path translation regression
build wsp_min2
# GetFileAttributes path normalization for Godot 4.3+ wide paths
build godot_path
# msvcrt _wfopen wide-path conversion (wcstombs -> win_utf16_to_utf8)
build wfopen_path
# window / message pump suite
build msgpump_test -luser32 -lgdi32
build strgl_test   -luser32 -lgdi32
build pump_min     -luser32 -lgdi32
# D3D11 swapchain-clear suite (swrast / WARP-style path)
build dbg10        -luser32 -lgdi32 -ld3d11 -ldxgi
build d3d_clear3   -luser32 -lgdi32 -ld3d11 -ldxgi

echo "Guests built into $OUT"
