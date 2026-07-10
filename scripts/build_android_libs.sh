#!/usr/bin/env bash
# Refresh the prebuilt Android native libraries shipped to host apps.
#
# Host apps consume libretro_emulator.so from resources/android/jniLibs/
# (copied into their build by the plugin's copy-assets hook) — they never
# compile the ares C++ themselves. Run this after any native change, then
# commit the updated .so files.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO_ROOT/resources/android/jniLibs"
STRIPPED="$REPO_ROOT/android/app/build/intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib"

cd "$REPO_ROOT/android"
./gradlew :app:assembleRelease --console=plain -q

for abi in arm64-v8a x86_64; do
    mkdir -p "$OUT/$abi"
    cp "$STRIPPED/$abi/libretro_emulator.so" "$OUT/$abi/"
    # libc++_shared is deliberately NOT shipped — the NativePHP host app's own
    # native build already packages it; a second copy fails APK packaging.
done

ls -la "$OUT"/*/
