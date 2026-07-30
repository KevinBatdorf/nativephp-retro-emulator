#!/usr/bin/env bash
# Fail when shipped copies drift from source. Exists because Phase 14 added
# emu_set_audio/emu_set_video to the source header and the copy the
# xcframework ships to consumers silently kept the old API.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FAIL=0

decls() {
    grep -E '^\s*(bool|void|int|size_t|uint32_t|double|const char\*|AresContext\*)\s+emu_' "$1" | sed 's/\s\+/ /g' | sort
}

SRC="$ROOT/ios/emulator_api.h"
for shipped in \
    "$ROOT/ios/headers/emulator_api.h" \
    "$ROOT"/build/RetroEmulator.xcframework/*/Headers/emulator_api.h \
    "$ROOT"/build/RetroEmulator.xcframework/*/RetroEmulator.framework/Headers/emulator_api.h; do
    [[ -f "$shipped" ]] || continue
    if ! diff <(decls "$SRC") <(decls "$shipped") > /dev/null; then
        echo "✗ shipped header drifted from source: ${shipped#"$ROOT"/}"
        diff <(decls "$SRC") <(decls "$shipped") | sed 's/^/    /' || true
        FAIL=1
    fi
done

if [[ $FAIL -eq 0 ]]; then
    echo "✓ shipped artifacts match source"
fi
exit $FAIL
