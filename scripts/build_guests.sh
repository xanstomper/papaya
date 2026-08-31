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
# ucrtbase.dll wide-string surface (Unity/Godot-Mono/.NET)
build ucrt_wide
# kernel32 file-mapping + string/misc batch
build kernel32_mapfile
# advapi32 registry create/delete-key + delete-value
build reg_create_delete -ladvapi32
# CRT runtime + formatting + comctl32/delay-load batch
build crt_runtime -lcomctl32
# _vsnprintf real varargs formatting through the CRT HLE
build vsnprintf_test
# NTDLL Rtl string/image/version helpers
build rtl_helpers -lntdll
# user32 dialog/menu/item helpers
build user32_dialog -luser32
# oleaut32 Variant + SafeArray (COM interop)
build oleaut_variant -loleaut32
# RT_STRING resource loading via LoadStringA/W (links loadstring.rc)
"$CC" -O1 -Wall -c "$REPO/tests/guests/loadstring.c" -o "$OUT/loadstring.o" -I.
x86_64-w64-mingw32-windres "$REPO/tests/guests/loadstring.rc" -O coff -o "$OUT/loadstring_res.o"
"$CC" -O1 -o "$OUT/loadstring.exe" "$OUT/loadstring.o" "$OUT/loadstring_res.o" -mwindows
# window / message pump suite
build msgpump_test -luser32 -lgdi32
build strgl_test   -luser32 -lgdi32
build pump_min     -luser32 -lgdi32
# D3D11 swapchain-clear suite (swrast / WARP-style path)
build dbg10        -luser32 -lgdi32 -ld3d11 -ldxgi
build d3d_clear3   -luser32 -lgdi32 -ld3d11 -ldxgi

echo "Guests built into $OUT"
