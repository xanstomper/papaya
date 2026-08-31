#!/usr/bin/env bash
# Run the guest verification suite under the Papaya runtime and assert:
#   1. every guest exits 0 within its timeout
#   2. strict-tier guests produce ZERO unresolved-import boot noise
# Usage: scripts/verify_guests.sh [guest_dir]
# Env:
#   PAPAYA_BIN       papaya executable (default build/src/app/papaya)
#   PAPAYA_RUNNER    command prefix used to launch papaya (e.g. "xvfb-run -a")
#   VERIFY_TIMEOUT   per-guest timeout seconds (default 60)
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
GUESTS="${1:-$REPO/build/guests}"
PAPAYA_BIN="${PAPAYA_BIN:-$REPO/build/src/app/papaya}"
RUNNER="${PAPAYA_RUNNER:-}"
TIMEOUT_S="${VERIFY_TIMEOUT:-60}"
STRICT="msvcrt_boot wstest wsprintf_boot wsp_min2 godot_path wfopen_path ucrt_wide kernel32_mapfile reg_create_delete crt_runtime vsnprintf_test loadstring rtl_helpers user32_dialog oleaut_variant"

[ -x "$PAPAYA_BIN" ] || { echo "error: papaya not built at $PAPAYA_BIN" >&2; exit 2; }
[ -d "$GUESTS" ] || { echo "error: guest dir not found: $GUESTS (run scripts/build_guests.sh)" >&2; exit 2; }

pass=0; fail=0; failed=""
for exe in "$GUESTS"/*.exe; do
    name="$(basename "$exe" .exe)"
    work="$(mktemp -d)"
    cp "$exe" "$work/"
    log="$work/run.log"
    (
        cd "$work"
        timeout "$TIMEOUT_S" $RUNNER "$PAPAYA_BIN" "$name.exe" >"$log" 2>&1
    )
    rc=$?
    noise=$(grep -aci 'unresolved-import' "$log" || true)
    status="ok"
    if [ $rc -ne 0 ]; then
        status="FAIL(exit=$rc)"; fail=$((fail+1)); failed="$failed $name"
    elif case " $STRICT " in *" $name "*) true;; *) false;; esac && [ "${noise:-0}" -ne 0 ]; then
        status="FAIL(boot-noise=$noise)"; fail=$((fail+1)); failed="$failed $name"
    else
        pass=$((pass+1))
    fi
    printf '%-16s %-22s unresolved=%s\n' "$name" "$status" "${noise:-0}"
    rm -rf "$work"
done

echo "-----------------------------"
echo "guest suite: $pass passed, $fail failed"
[ $fail -eq 0 ] || { echo "failed:$failed" >&2; exit 1; }
