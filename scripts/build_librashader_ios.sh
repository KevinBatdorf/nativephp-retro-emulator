#!/usr/bin/env bash
# Build the librashader Metal runtime as static libraries for iOS.
#
# The iOS side links librashader STATICALLY (merged into the RetroEmulator
# framework binary by build_xcframework.sh) — iOS only loads code-signed
# embedded dylibs, so the Android .so approach doesn't carry over. Same pinned
# tag as Android (build_librashader_android.sh) so the vendored header at
# native/vendor/librashader/ never skews from either binary.
#
# WHY --features stable / --no-default-features runtime-metal: see the Android
# script — same reasoning, Metal runtime instead of Vulkan.
#
# Outputs:
#   build/librashader-ios/ios-arm64/liblibrashader.a            (device)
#   build/librashader-ios/ios-arm64_x86_64-simulator/liblibrashader.a
set -euo pipefail
shopt -s inherit_errexit

LIBRASHADER_TAG="librashader-v0.12.0"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$REPO_ROOT/build/librashader-src"
OUT="$REPO_ROOT/build/librashader-ios"

# --- ensure the Rust toolchain bits ------------------------------------------
rustup target add aarch64-apple-ios aarch64-apple-ios-sim x86_64-apple-ios >/dev/null

# --- pin + fetch the source ---------------------------------------------------
if [[ ! -d "$SRC_DIR/.git" ]]; then
    mkdir -p "$(dirname "$SRC_DIR")"
    git clone --depth 1 --branch "$LIBRASHADER_TAG" \
        https://github.com/SnowflakePowered/librashader.git "$SRC_DIR"
else
    git -C "$SRC_DIR" fetch --depth 1 origin tag "$LIBRASHADER_TAG" >/dev/null 2>&1 || true
    git -C "$SRC_DIR" checkout -q "$LIBRASHADER_TAG"
fi

# Upstream 0.12.0 bug: the FilterChainMetal import is gated on
# target_os = "macos" while the type alias it feeds is gated on
# target_vendor = "apple", so iOS targets hit E0412. Align the import's gate.
sed -i '' \
    's/#\[cfg(all(target_os = "macos", feature = "runtime-metal"))\]/#[cfg(all(target_vendor = "apple", feature = "runtime-metal"))]/' \
    "$SRC_DIR/librashader-capi/src/ctypes.rs"

# --- build the Metal-only staticlib per target --------------------------------
# Deployment target must match the xcframework's (build_xcframework.sh): the
# vendored C++ (spirv-cross via cc-rs) otherwise compiles against the host
# SDK's newest libc++ and emits calls (e.g. std::__hash_memory) that older
# runtimes don't export — the app then dies at dyld with "Symbol missing".
export IPHONEOS_DEPLOYMENT_TARGET=16.0

# Sets BUILT_A instead of echoing: $() subshells don't inherit errexit on
# macOS's bash 3.2, so a cargo failure inside one would go unnoticed.
build_target() {
    local target="$1"
    ( cd "$SRC_DIR" && cargo build --release -p librashader-capi \
        --no-default-features --features runtime-metal --features stable \
        --target "$target" )
    BUILT_A="$SRC_DIR/target/$target/release/liblibrashader_capi.a"
}

build_target aarch64-apple-ios;     DEVICE_A="$BUILT_A"
build_target aarch64-apple-ios-sim; SIM_ARM_A="$BUILT_A"
build_target x86_64-apple-ios;      SIM_X86_A="$BUILT_A"

mkdir -p "$OUT/ios-arm64" "$OUT/ios-arm64_x86_64-simulator"
cp "$DEVICE_A" "$OUT/ios-arm64/liblibrashader.a"
lipo -create "$SIM_ARM_A" "$SIM_X86_A" \
    -output "$OUT/ios-arm64_x86_64-simulator/liblibrashader.a"

# Refresh the vendored C header from the SAME tag so it never skews.
HDR="$REPO_ROOT/native/vendor/librashader"
mkdir -p "$HDR"
cp "$SRC_DIR/include/librashader.h" "$SRC_DIR/include/librashader_ld.h" "$HDR/"

for f in "$OUT"/*/liblibrashader.a; do
    echo "built $f ($(stat -f%z "$f") bytes)"
done
