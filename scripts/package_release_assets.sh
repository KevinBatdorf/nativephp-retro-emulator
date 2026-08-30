#!/usr/bin/env bash
# package_release_assets.sh — zip the prebuilt native cores for a GitHub
# release and pin their sha256s into resources/native-assets.json.
#
# Run after scripts/build_android_libs.sh and scripts/build_xcframework.sh,
# before tagging. Then upload the zips to the release the manifest names:
#   gh release create vX.Y.Z build/release-assets/*.zip
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/release-assets"
RELEASE="v$(php -r 'echo json_decode(file_get_contents($argv[1]))->version;' "$ROOT/nativephp.json")"

JNILIBS="$ROOT/resources/android/jniLibs"
XCFW="$ROOT/build/RetroEmulator.xcframework"

[ -d "$JNILIBS" ] || { echo "missing $JNILIBS — run scripts/build_android_libs.sh first" >&2; exit 1; }
[ -d "$XCFW" ] || { echo "missing $XCFW — run scripts/build_xcframework.sh first" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"

(cd "$JNILIBS" && zip -qr "$OUT/android-jniLibs.zip" .)
(cd "$ROOT/build" && zip -qr "$OUT/RetroEmulator.xcframework.zip" RetroEmulator.xcframework)

ANDROID_SHA=$(shasum -a 256 "$OUT/android-jniLibs.zip" | cut -d' ' -f1)
IOS_SHA=$(shasum -a 256 "$OUT/RetroEmulator.xcframework.zip" | cut -d' ' -f1)

php -r '
$m = json_decode(file_get_contents($argv[1]), true);
$m["release"] = $argv[2];
$m["assets"]["android-jniLibs.zip"]["sha256"] = $argv[3];
$m["assets"]["RetroEmulator.xcframework.zip"]["sha256"] = $argv[4];
file_put_contents($argv[1], json_encode($m, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES)."\n");
' "$ROOT/resources/native-assets.json" "$RELEASE" "$ANDROID_SHA" "$IOS_SHA"

echo "✓ $OUT/android-jniLibs.zip ($ANDROID_SHA)"
echo "✓ $OUT/RetroEmulator.xcframework.zip ($IOS_SHA)"
echo "✓ resources/native-assets.json pinned to $RELEASE"
