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

# The packaged Headers/ are a staged copy — refresh from the source of truth
# so they can never drift from ios/ares_ios_api.h (check_artifacts verifies).
cp "$IOS_SRC/ares_ios_api.h" "$HEADERS_DIR/ares_ios_api.h"
cp "$IOS_SRC/librashader_metal_shim.h" "$HEADERS_DIR/librashader_metal_shim.h"

DEVICE_SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
SIM_SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"

log() { echo "▸ $*" >&2; }

# cmake_build LABEL [cmake-args...]  — sets BUILT_LIB to the built .a.
# Runs at top level (not in a $() subshell): command substitutions don't
# inherit errexit on macOS's bash 3.2, so a compile failure inside $() would
# silently package stale slices.
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
    BUILT_LIB="$build_dir/libretro_emulator_ios.a"
}

# make_moltenvk_obj SLICE_ID OUT_OBJ — partial-link the vendored MoltenVK
# static lib for a slice into ONE relocatable object whose only exported
# symbol is the ICD entrypoint vk_icdGetInstanceProcAddr. Localizing the
# standard vk* functions is what makes static MoltenVK coexist with volk,
# which defines identically-named global function-pointer VARIABLES (volk.c)
# that would otherwise collide at app link. ios/moltenvk_loader.cpp seeds
# Granite's loader from the ICD entrypoint at runtime.
make_moltenvk_obj() {
    local slice="$1" out="$2"
    local src="$IOS_SRC/vendor/moltenvk/$slice/libMoltenVK.a"
    if [[ ! -f "$src" ]]; then
        echo "error: $src not found — vendored MoltenVK static lib missing" >&2
        exit 1
    fi

    local sdk platform sdk_ver
    if [[ "$slice" == *simulator* ]]; then
        sdk="$SIM_SDK"; platform="ios-simulator"
        sdk_ver="$(xcrun --sdk iphonesimulator --show-sdk-version)"
    else
        sdk="$DEVICE_SDK"; platform="ios"
        sdk_ver="$(xcrun --sdk iphoneos --show-sdk-version)"
    fi

    local workdir exports
    workdir="$(dirname "$out")"
    mkdir -p "$workdir"
    exports="$workdir/mvk_exports.txt"
    printf '_vk_icdGetInstanceProcAddr\n' > "$exports"

    # The vendored archive may be fat (simulator: arm64 + x86_64); ld -r is
    # single-arch, so localize per arch and lipo the results back together.
    local -a objs=()
    local arch
    for arch in $(lipo -archs "$src"); do
        ld -r -arch "$arch" \
           -platform_version "$platform" "$DEPLOYMENT_TARGET" "$sdk_ver" \
           -syslibroot "$sdk" \
           -all_load "$src" \
           -exported_symbols_list "$exports" \
           -o "$workdir/mvk-$arch.o"
        objs+=("$workdir/mvk-$arch.o")
    done
    if [[ ${#objs[@]} -eq 1 ]]; then
        cp "${objs[0]}" "$out"
    else
        lipo -create "${objs[@]}" -output "$out"
    fi
}

# make_xcframework OUTPUT -library <.a> -headers <dir> [-library ... -headers ...]
# Assembles an xcframework of STATIC FRAMEWORKS — CocoaPods' vendored_frameworks
# needs RetroEmulator.framework per slice (a bare .a + Headers doesn't link);
# the demo consumes this via a local podspec. Assembled manually so the layout
# is deterministic across Xcode versions.
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

        local fw_dir="$xcfw/$slice_id/RetroEmulator.framework"
        mkdir -p "$fw_dir/Headers" "$fw_dir/Modules"

        # Merge the librashader Metal staticlib for this slice into the
        # framework binary — the shim (librashader_metal_shim.mm) calls it and
        # consumers must not need a second vendored library. Built by
        # scripts/build_librashader_ios.sh; missing = hard error, a framework
        # without it dies at runtime on the first SetShader.
        local lrs_a="$BUILD_ROOT/librashader-ios/$slice_id/liblibrashader.a"
        if [[ ! -f "$lrs_a" ]]; then
            echo "error: $lrs_a not found — run scripts/build_librashader_ios.sh first" >&2
            exit 1
        fi

        # MoltenVK rides inside the framework binary the same way librashader
        # does — consumers get N64's Vulkan implementation with zero wiring.
        local mvk_obj="$BUILD_ROOT/moltenvk/$slice_id/moltenvk.o"
        make_moltenvk_obj "$slice_id" "$mvk_obj"

        libtool -static -o "$fw_dir/RetroEmulator" "$lib_path" "$lrs_a" "$mvk_obj"

        cp -r "$hdr_path/." "$fw_dir/Headers/"
        # The framework module map supersedes the plain one shipped in headers/.
        rm -f "$fw_dir/Headers/module.modulemap"
        cat > "$fw_dir/Modules/module.modulemap" <<'EOF'
framework module RetroEmulator {
    header "ares_ios_api.h"
    header "librashader_metal_shim.h"
    export *
}
EOF

        local supported_platform="iPhoneOS"
        $is_sim && supported_platform="iPhoneSimulator"
        /usr/bin/python3 - "$fw_dir/Info.plist" "$supported_platform" "$DEPLOYMENT_TARGET" <<'PYEOF'
import sys, plistlib
out_path, platform, min_os = sys.argv[1], sys.argv[2], sys.argv[3]
plist = {
    "CFBundleExecutable": "RetroEmulator",
    "CFBundleIdentifier": "com.kevinbatdorf.RetroEmulator",
    "CFBundleInfoDictionaryVersion": "6.0",
    "CFBundleName": "RetroEmulator",
    "CFBundlePackageType": "FMWK",
    "CFBundleShortVersionString": "0.1.0",
    "CFBundleSupportedPlatforms": [platform],
    "CFBundleVersion": "1",
    "MinimumOSVersion": min_os,
}
with open(out_path, "wb") as f:
    plistlib.dump(plist, f, fmt=plistlib.FMT_XML, sort_keys=True)
PYEOF

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
        "BinaryPath":           "RetroEmulator.framework/RetroEmulator",
        "LibraryIdentifier":    e["slice"],
        "LibraryPath":          "RetroEmulator.framework",
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
cmake_build sim-arm64 \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_SYSROOT="$SIM_SDK"
SIM_ARM64_LIB="$BUILT_LIB"

cmake_build sim-x86_64 \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_OSX_SYSROOT="$SIM_SDK"
SIM_X86_LIB="$BUILT_LIB"

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
    cmake_build device \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_SYSROOT="$DEVICE_SDK"
    DEVICE_LIB="$BUILT_LIB"

    log "Assembling xcframework (device + simulator) …"
    make_xcframework "$XCFW_OUTPUT" \
        -library "$DEVICE_LIB" -headers "$HEADERS_DIR" \
        -library "$SIM_FAT"    -headers "$HEADERS_DIR"
fi

log "✓ RetroEmulator.xcframework → $XCFW_OUTPUT"
find "$XCFW_OUTPUT" -name "RetroEmulator" -type f | while read f; do
    log "  $(basename "$(dirname "$(dirname "$f")")") — $(du -sh "$f" | cut -f1)"
done
