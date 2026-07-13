#!/usr/bin/env bash
# Build the librashader Vulkan runtime as a prebuilt Android shared library.
#
# librashader (RetroArch "slang" shader runtime) is NOT compiled from source in
# the host build — like ares, it ships as a prebuilt .so in
# resources/android/jniLibs/<abi>/liblibrashader.so (the plugin's copy-assets
# hook ships it to hosts; the dev-harness links it via an imported target).
#
# WHY v0.12.0: librashader-vk 0.5.1 hit VK_ERROR_OUT_OF_POOL_MEMORY creating any
# filter chain on Adreno (the Thor) — fixed upstream by 71733a9 "Fix descriptor
# count" (librashader-runtime-vk), first released in v0.9.1. We pin the latest
# (v0.12.0, all Vulkan fixes). This script also refreshes our own copy of the C
# header (native/vendor/librashader/) from the SAME tag, so the header we compile
# against and the .so we link are always the same version — no skew. (The ares
# tree's thirdparty/librashader header is ares' desktop concern; unused here.)
#
# WHY --features stable: librashader-reflect gates the nightly
# `impl_trait_in_assoc_type` feature behind its `stable` cargo feature (E0554 on
# a stable toolchain otherwise). WHY --no-default-features runtime-vulkan: Android
# needs only the Vulkan runtime; the default `runtime-all` pulls in GL/D3D/Metal.
#
# Run after a toolchain change or a deliberate librashader bump, then commit the
# updated .so files (.gitignore negates *.so under resources/android/jniLibs).
set -euo pipefail

LIBRASHADER_TAG="librashader-v0.12.0"
ANDROID_PLATFORM=26   # matches android/app/build.gradle.kts minSdk
ABIS=(arm64-v8a x86_64)

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$REPO_ROOT/build/librashader-src"
OUT="$REPO_ROOT/resources/android/jniLibs"

# --- locate the NDK (match the app's pinned ndkVersion when possible) ---------
NDK="${ANDROID_NDK_HOME:-}"
if [[ -z "$NDK" ]]; then
    SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
    NDK="$(/bin/ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1)"
fi
[[ -d "$NDK" ]] || { echo "error: Android NDK not found (set ANDROID_NDK_HOME)"; exit 1; }
export ANDROID_NDK_HOME="$NDK"
HOST_TAG="$(/bin/ls -d "$NDK"/toolchains/llvm/prebuilt/* | head -1)"
STRIP="$HOST_TAG/bin/llvm-strip"
echo "NDK: $NDK"

# --- ensure the Rust toolchain bits ------------------------------------------
rustup target add aarch64-linux-android x86_64-linux-android >/dev/null
cargo ndk --version >/dev/null 2>&1 || cargo install cargo-ndk

# --- pin + fetch the source ---------------------------------------------------
if [[ ! -d "$SRC_DIR/.git" ]]; then
    mkdir -p "$(dirname "$SRC_DIR")"
    git clone --depth 1 --branch "$LIBRASHADER_TAG" \
        https://github.com/SnowflakePowered/librashader.git "$SRC_DIR"
else
    git -C "$SRC_DIR" fetch --depth 1 origin tag "$LIBRASHADER_TAG" >/dev/null 2>&1 || true
    git -C "$SRC_DIR" checkout -q "$LIBRASHADER_TAG"
fi

# --- cross-build the Vulkan-only cdylib for each ABI --------------------------
BUILD_OUT="$SRC_DIR/jniLibs-out"
rm -rf "$BUILD_OUT"
( cd "$SRC_DIR" && cargo ndk --platform "$ANDROID_PLATFORM" \
    $(printf -- '-t %s ' "${ABIS[@]}") -o "$BUILD_OUT" \
    build --release -p librashader-capi \
    --no-default-features --features runtime-vulkan --features stable )

# --- strip + install ----------------------------------------------------------
# Committed source of truth: resources/android/jniLibs (shipped to hosts via the
# copy-assets hook). The dev-harness (android/) needs its own copy under its
# default jniLibs srcDir so gradle packages it and CMake's imported target
# resolves — that copy is gitignored (regenerated here).
DEVHARNESS="$REPO_ROOT/android/app/src/main/jniLibs"
for abi in "${ABIS[@]}"; do
    mkdir -p "$OUT/$abi" "$DEVHARNESS/$abi"
    "$STRIP" --strip-unneeded "$BUILD_OUT/$abi/liblibrashader_capi.so" \
        -o "$OUT/$abi/liblibrashader.so"
    cp "$OUT/$abi/liblibrashader.so" "$DEVHARNESS/$abi/liblibrashader.so"
    echo "installed $OUT/$abi/liblibrashader.so ($(stat -f%z "$OUT/$abi/liblibrashader.so") bytes)"
done

# Refresh the vendored C header from the SAME tag so it never skews from the .so.
HDR="$REPO_ROOT/native/vendor/librashader"
mkdir -p "$HDR"
cp "$SRC_DIR/include/librashader.h" "$SRC_DIR/include/librashader_ld.h" "$HDR/"
echo "refreshed vendored header at native/vendor/librashader/"
echo "done — commit the updated .so files + native/vendor/librashader/*.h"
