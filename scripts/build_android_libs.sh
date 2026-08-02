#!/usr/bin/env bash
# Refresh the prebuilt Android native libraries shipped to host apps.
#
# Host apps consume libretro_emulator.so from resources/android/jniLibs/
# (copied into their build by the plugin's copy-assets hook) — they never
# compile the ares C++ themselves. Run this after any native change, then
# commit the updated .so files.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# The ares submodule stays pristine upstream; local fixes live in patches/
# and apply idempotently before every build (--check fails once applied).
for p in "$REPO_ROOT"/patches/ares-*.patch; do
  [ -e "$p" ] || continue
  if git -C "$REPO_ROOT/ares" apply --check "$p" 2>/dev/null; then
    git -C "$REPO_ROOT/ares" apply "$p"
    echo "applied $(basename "$p")"
  fi
done
OUT="$REPO_ROOT/resources/android/jniLibs"
STRIPPED="$REPO_ROOT/android/app/build/intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib"

cd "$REPO_ROOT/android"
./gradlew :app:assembleRelease --console=plain -q

for abi in arm64-v8a x86_64; do
    mkdir -p "$OUT/$abi"
    # The modular set: engine-neutral frontend + one library per backend +
    # one module per ares core. The copy-assets hook filters core modules by
    # the host's config/retro-emulator.php selection.
    # The copies below only ever add — anything already in OUT ships to
    # every host app, so clear retired library names first.
    rm -f "$OUT/$abi"/libretro_ares.so "$OUT/$abi"/libretro_core_*.so
    cp "$STRIPPED/$abi/libretro_emulator.so" "$OUT/$abi/"
    cp "$STRIPPED/$abi"/libbackend_*.so "$OUT/$abi/"
    cp "$STRIPPED/$abi"/libares_core_*.so "$OUT/$abi/"
    # libc++_shared is deliberately NOT shipped — the NativePHP host app's own
    # native build already packages it; a second copy fails APK packaging.
done

ls -la "$OUT"/*/
