# Guest Verification Suite

Real mingw-w64 guest executables that run under the Papaya runtime and
assert behavior from *inside* the guest. Build with
`scripts/build_guests.sh` (needs `gcc-mingw-w64-x86-64`), verify with
`scripts/verify_guests.sh`. Every guest must exit 0. Guests in the strict
tier must additionally produce **zero unresolved-import boot noise**.

| Guest | Tier | Covers |
|---|---|---|
| `msvcrt_boot.c` | strict | CRT boot imports: DBCS lead bytes, codepage/MB_CUR_MAX accessors, `_lock/_unlock`, `fflush`, `localeconv` (Windows lconv layout), `strerror` |
| `wstest.c` | strict | USER32 `wsprintfA` core formatting (`%5d`, `%-5d`, `%05d`, `%x`, `%X`, `%c`) |
| `wsprintf_boot.c` | strict | Full documented wsprintfA spec: register **and stack** varargs, size prefixes (`ld lu lx lX hd hu`), `Ix/IX` 64-bit hex, `#`/`0`/`-` flags, width, precision, `%p`, `%%` |
| `wsp_min2.c` | strict | msvcrt FILE\* lifecycle: `fopen` (incl. `C:\` path translation), `fprintf`, `fflush`, `fclose` — regression for the host FILE\* registry |
| `godot_path.c` | strict | `GetFileAttributesW` path normalization for Godot 4.3+ wide/device paths (`\\?\C:\...` and `C:\...` resolve to CWD-relative files) — regression for `normalize_win_path` |
| `wfopen_path.c` | strict | msvcrt `_wfopen` wide-path conversion via `C:\` path (UTF-16→UTF-8, not `wcstombs`) — regression locking in Godot `.pck` open |
| `msgpump_test.c` | loose | Window class registration, creation, message pump |
| `strgl_test.c` | loose | String/heap CRT family through the HLE |
| `pump_min.c` | loose | Minimal WM_PAINT paint cycle (BeginPaint/EndPaint) |
| `dbg10.c` | loose | D3D11 device/swapchain boot (swrast path) |
| `d3d_clear3.c` | loose | D3D11 clear + present round-trip |

Loose-tier guests must still exit 0; they may log unresolved-import
warnings for APIs not yet implemented (that noise is tracked by
`docs/TOP_1000_APIS.md` and `docs/GAME_API_COVERAGE.md`).
