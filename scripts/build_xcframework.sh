#!/usr/bin/env bash
# build_xcframework.sh — compile ares for iOS and produce RetroEmulator.xcframework
#
# Outputs: build/RetroEmulator.xcframework  (device + simulator slices)
#
# Usage:
#   ./scripts/build_xcframework.sh            # device + simulator
#   ./scripts/build_xcframework.sh --sim-only # simulator only (faster)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
IOS_SRC="$REPO_ROOT/ios"
BUILD_ROOT="$REPO_ROOT/build"
HEADERS_DIR="$IOS_SRC/headers"
XCFW_OUTPUT="$BUILD_ROOT/RetroEmulator.xcframework"
DEPLOYMENT_TARGET="16.0"

SIM_ONLY=false
for arg in "$@"; do [[ "$arg" == "--sim-only" ]] && SIM_ONLY=true; done

DEVICE_SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
SIM_SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"

log() { echo "▸ $*" >&2; }

# cmake_build LABEL [cmake-args...]  — prints path to the built .a on stdout.
cmake_build() {
    local label="$1"; shift
    local build_dir="$BUILD_ROOT/$label"
    log "Configuring $label …"
    cmake -S "$IOS_SRC" -B "$build_dir" \
        -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        "$@" >&2
    log "Building $label …"
    cmake --build "$build_dir" --parallel "$(sysctl -n hw.logicalcpu)" >&2
    echo "$build_dir/libretro_emulator_ios.a"
}

# make_xcframework OUTPUT -library <.a> -headers <dir> [-library ... -headers ...]
# Assembles the xcframework directory structure manually.
# xcodebuild -create-xcframework is broken on Xcode 26.5 beta
# (IDESimulatorFoundation plugin ABI mismatch with DVTDownloads).
make_xcframework() {
    local xcfw="$1"; shift
    rm -rf "$xcfw"
    mkdir -p "$xcfw"

    # Collect (lib, headers) pairs from arguments.
    local -a libs=()
    local -a hdrs=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -library) libs+=("$2"); shift 2 ;;
            -headers) hdrs+=("$2"); shift 2 ;;
        esac
    done

    local n="${#libs[@]}"
    local plist_json="["

    for ((i=0; i<n; i++)); do
        local lib_path="${libs[$i]}"
        local hdr_path="${hdrs[$i]}"

        # Get architectures from lipo (space-separated, e.g. "arm64 x86_64").
        local archs_raw
        archs_raw="$(lipo -archs "$lib_path" 2>/dev/null)"

        local is_sim=false
        [[ "$lib_path" == *sim* ]] && is_sim=true

        # Slice identifier: ios-arm64_x86_64-simulator or ios-arm64
        local sorted_archs
        sorted_archs="$(echo "$archs_raw" | tr ' ' '\n' | sort | tr '\n' '_' | sed 's/_$//')"
        local slice_id
        if $is_sim; then
            slice_id="ios-${sorted_archs}-simulator"
        else
            slice_id="ios-${sorted_archs}"
        fi

        local slice_dir="$xcfw/$slice_id"
        mkdir -p "$slice_dir/Headers"
        cp "$lib_path" "$slice_dir/libretro_emulator_ios.a"
        cp -r "$hdr_path/." "$slice_dir/Headers/"

        # Build arch JSON array for Python.
        local arch_json="["
        for a in $archs_raw; do
            arch_json+="\"$a\","
        done
        arch_json="${arch_json%,}]"

        local variant_json="null"
        $is_sim && variant_json="\"simulator\""

        [[ $i -gt 0 ]] && plist_json+=","
        plist_json+="{\"slice\":\"$slice_id\",\"archs\":$arch_json,\"variant\":$variant_json}"
    done
    plist_json+="]"

    # Use system Python (3.9, always present on macOS) for plist generation.
    # Avoid Homebrew python3 which may have broken native extensions.
    /usr/bin/python3 - "$xcfw/Info.plist" "$plist_json" <<'PYEOF'
import sys, json, plistlib
from plistlib import dump as plist_dump

out_path = sys.argv[1]
entries  = json.loads(sys.argv[2])

libs = []
for e in entries:
    d = {
        "HeadersPath":          "Headers",
        "LibraryIdentifier":    e["slice"],
        "LibraryPath":          "libretro_emulator_ios.a",
        "SupportedArchitectures": e["archs"],
        "SupportedPlatform":    "ios",
    }
    if e["variant"]:
        d["SupportedPlatformVariant"] = e["variant"]
    libs.append(d)

plist = {
    "AvailableLibraries":   libs,
    "CFBundlePackageType":  "XFWK",
    "XCFrameworkFormatVersion": "1.0",
}
with open(out_path, "wb") as f:
    plist_dump(plist, f, fmt=plistlib.FMT_XML, sort_keys=True)
PYEOF

    log "Created $xcfw"
}

mkdir -p "$BUILD_ROOT"
rm -rf "$XCFW_OUTPUT"

# ---------------------------------------------------------------------------
# Simulator — arm64 + x86_64 (fat binary via lipo)
# ---------------------------------------------------------------------------
SIM_ARM64_LIB="$(cmake_build sim-arm64 \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_SYSROOT="$SIM_SDK")"

SIM_X86_LIB="$(cmake_build sim-x86_64 \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_OSX_SYSROOT="$SIM_SDK")"

SIM_FAT="$BUILD_ROOT/sim-fat/libretro_emulator_ios.a"
mkdir -p "$(dirname "$SIM_FAT")"
log "Merging simulator slices …"
lipo -create "$SIM_ARM64_LIB" "$SIM_X86_LIB" -output "$SIM_FAT"

if $SIM_ONLY; then
    log "Assembling xcframework (simulator only) …"
    make_xcframework "$XCFW_OUTPUT" \
        -library "$SIM_FAT"  -headers "$HEADERS_DIR"
else
    # -------------------------------------------------------------------------
    # Device — arm64
    # -------------------------------------------------------------------------
    DEVICE_LIB="$(cmake_build device \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_SYSROOT="$DEVICE_SDK")"

    log "Assembling xcframework (device + simulator) …"
    make_xcframework "$XCFW_OUTPUT" \
        -library "$DEVICE_LIB" -headers "$HEADERS_DIR" \
        -library "$SIM_FAT"    -headers "$HEADERS_DIR"
fi

log "✓ RetroEmulator.xcframework → $XCFW_OUTPUT"
find "$XCFW_OUTPUT" -name "*.a" | while read f; do
    log "  $(basename "$(dirname "$f")") — $(du -sh "$f" | cut -f1)"
done
