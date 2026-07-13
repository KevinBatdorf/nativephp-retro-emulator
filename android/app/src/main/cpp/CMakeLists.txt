cmake_minimum_required(VERSION 3.22)

project(retro_emulator VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Suppress C++ module scanning (not supported by NDK's CMake integration)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# ares submodule root — relative to this CMakeLists.txt
get_filename_component(ARES_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../ares"
    ABSOLUTE
)

# Shared (platform-agnostic) native sources — pak builders, system registry.
get_filename_component(NATIVE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../native"
    ABSOLUTE
)

# ---------------------------------------------------------------------------
# Embedded firmware — generated from blobs in the ares submodule
# (GB boot ROM, MD TMSS/SVP, SFC ipl.rom + boards.bml).
# ---------------------------------------------------------------------------
set(FIRMWARE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
    OUTPUT "${FIRMWARE_DIR}/embedded_firmware.cpp" "${FIRMWARE_DIR}/embedded_firmware.hpp"
    COMMAND python3 "${NATIVE_ROOT}/../scripts/generate_firmware.py"
            "${ARES_ROOT}" "${FIRMWARE_DIR}"
    DEPENDS "${NATIVE_ROOT}/../scripts/generate_firmware.py"
    COMMENT "Generating embedded firmware from ares submodule"
)

# ---------------------------------------------------------------------------
# sljit — JIT allocator used by nall
# ---------------------------------------------------------------------------
add_library(sljit STATIC
    "${ARES_ROOT}/thirdparty/sljit/sljit_src/sljitLir.c"
)
target_include_directories(sljit PUBLIC "${ARES_ROOT}/thirdparty")
target_compile_definitions(sljit PUBLIC
    SLJIT_HAVE_CONFIG_PRE=1
    SLJIT_HAVE_CONFIG_POST=1
)

# ---------------------------------------------------------------------------
# libco — cooperative threading used by the ares scheduler
# ---------------------------------------------------------------------------
add_library(libco STATIC
    "${ARES_ROOT}/libco/libco.c"
)
if(ANDROID_ABI STREQUAL "arm64-v8a")
    target_sources(libco PRIVATE "${ARES_ROOT}/libco/aarch64.c")
elseif(ANDROID_ABI STREQUAL "x86_64")
    target_sources(libco PRIVATE "${ARES_ROOT}/libco/amd64.c")
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    target_sources(libco PRIVATE "${ARES_ROOT}/libco/arm.c")
else()
    target_sources(libco PRIVATE "${ARES_ROOT}/libco/sjlj.c")
endif()
target_include_directories(libco INTERFACE "${ARES_ROOT}")

# ---------------------------------------------------------------------------
# nall — ares support library (header-heavy, small .cpp compile units)
# ---------------------------------------------------------------------------
add_library(nall OBJECT
    "${ARES_ROOT}/nall/nall/nall.cpp"
    "${ARES_ROOT}/nall/nall/sljitAllocator.cpp"
)
target_include_directories(nall PUBLIC "${ARES_ROOT}/nall")
target_link_libraries(nall PUBLIC sljit)
target_compile_definitions(nall PUBLIC BUILD_RELEASE)
target_link_libraries(nall PUBLIC log)

# ---------------------------------------------------------------------------
# ares core — the emulation framework (node tree, scheduler, platform)
# ---------------------------------------------------------------------------
add_library(ares_core STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/ares_android.cpp"
    "${ARES_ROOT}/ares/ares/memory/fixed-allocator.cpp"
)
target_include_directories(ares_core PUBLIC
    "${ARES_ROOT}"
    "${ARES_ROOT}/ares"
)
target_link_libraries(ares_core PUBLIC libco nall)
target_compile_definitions(ares_core PUBLIC
    PROFILE_PERFORMANCE
    BUILD_RELEASE
)

# ---------------------------------------------------------------------------
# sfc_core — Super Famicom / SNES system (unity build via sfc.cpp)
#
# sfc.cpp (via sfc.hpp) includes the processor .hpp headers (struct decls)
# inside the ares::SuperFamicom namespace context.  The actual method bodies
# live in separate processor unity-build .cpp files that must be compiled as
# their own TUs and linked alongside sfc.cpp.
#
# Required processors for SFC:
#   wdc65816  — main CPU (65816)
#   spc700    — audio CPU (SPC700)
#   gsu       — SuperFX coprocessor
#   hg51b     — Hitachi DSP coprocessor (SA1 / HiROM)
#   upd96050  — NEC DSP coprocessor (DSP-1 through DSP-4)
# ---------------------------------------------------------------------------
set(PROC "${ARES_ROOT}/ares/component/processor")
add_library(sfc_core STATIC
    "${ARES_ROOT}/ares/sfc/sfc.cpp"
    "${PROC}/arm7tdmi/arm7tdmi.cpp"
    "${PROC}/wdc65816/wdc65816.cpp"
    "${PROC}/spc700/spc700.cpp"
    "${PROC}/gsu/gsu.cpp"
    "${PROC}/hg51b/hg51b.cpp"
    "${PROC}/upd96050/upd96050.cpp"
)
target_include_directories(sfc_core PUBLIC
    # stubs/ must come BEFORE ${ARES_ROOT}/ares so our ares/resource/resource.hpp
    # shadows the missing sourcery-generated version in the ares submodule.
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${ARES_ROOT}/ares"          # for <sfc/sfc.hpp>, <component/processor/…>
)
# Inherit ARES_ROOT and nall includes through ares_core's PUBLIC interface.
target_link_libraries(sfc_core PUBLIC ares_core)
target_compile_definitions(sfc_core PUBLIC
    PROFILE_PERFORMANCE
    BUILD_RELEASE
    CORE_SFC
)

# ---------------------------------------------------------------------------
# ymfm — Yamaha FM synthesis library (thirdparty), used by the FC core (EPSM).
# ---------------------------------------------------------------------------
add_library(ymfm STATIC
    "${ARES_ROOT}/thirdparty/ymfm/src/ymfm_adpcm.cpp"
    "${ARES_ROOT}/thirdparty/ymfm/src/ymfm_misc.cpp"
    "${ARES_ROOT}/thirdparty/ymfm/src/ymfm_opl.cpp"
    "${ARES_ROOT}/thirdparty/ymfm/src/ymfm_opm.cpp"
    "${ARES_ROOT}/thirdparty/ymfm/src/ymfm_opn.cpp"
    "${ARES_ROOT}/thirdparty/ymfm/src/ymfm_opq.cpp"
    "${ARES_ROOT}/thirdparty/ymfm/src/ymfm_opz.cpp"
    "${ARES_ROOT}/thirdparty/ymfm/src/ymfm_pcm.cpp"
    "${ARES_ROOT}/thirdparty/ymfm/src/ymfm_ssg.cpp"
)
target_include_directories(ymfm PUBLIC "${ARES_ROOT}/thirdparty/ymfm/src")

# ---------------------------------------------------------------------------
# fc_core — Famicom / NES system (unity build via fc.cpp)
#   mos6502 — main CPU
# ---------------------------------------------------------------------------
add_library(fc_core STATIC
    "${ARES_ROOT}/ares/fc/fc.cpp"
    "${PROC}/mos6502/mos6502.cpp"
)
target_include_directories(fc_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${ARES_ROOT}/ares"
)
target_link_libraries(fc_core PUBLIC ares_core ymfm)
target_compile_definitions(fc_core PUBLIC
    PROFILE_PERFORMANCE
    BUILD_RELEASE
    CORE_FC
)

# ---------------------------------------------------------------------------
# gb_core — Game Boy system (unity build via gb.cpp)
#   sm83     — main CPU
#   m93lcx6  — EEPROM used by some cartridges
# ---------------------------------------------------------------------------
add_library(gb_core STATIC
    "${ARES_ROOT}/ares/gb/gb.cpp"
    "${PROC}/sm83/sm83.cpp"
    "${ARES_ROOT}/ares/component/eeprom/m93lcx6/m93lcx6.cpp"
)
target_include_directories(gb_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${ARES_ROOT}/ares"
)
target_link_libraries(gb_core PUBLIC ares_core)
target_compile_definitions(gb_core PUBLIC
    PROFILE_PERFORMANCE
    BUILD_RELEASE
    CORE_GB
)

# ---------------------------------------------------------------------------
# md_core — Mega Drive / Genesis system (unity build via md.cpp)
#   m68000  — main CPU
#   z80     — audio CPU
#   sh2     — 32X coprocessor (compiled in via m32x.cpp)
#   ssp1601 — SVP coprocessor (Virtua Racing)
#   sn76489 / ym2612 — audio chips
#   m24c    — EEPROM used by some cartridges
# ---------------------------------------------------------------------------
add_library(md_core STATIC
    "${ARES_ROOT}/ares/md/md.cpp"
    "${PROC}/m68000/m68000.cpp"
    "${PROC}/z80/z80.cpp"
    "${PROC}/sh2/sh2.cpp"
    "${PROC}/ssp1601/ssp1601.cpp"
    "${ARES_ROOT}/ares/component/audio/sn76489/sn76489.cpp"
    "${ARES_ROOT}/ares/component/audio/ym2413/ym2413.cpp"
    "${ARES_ROOT}/ares/component/audio/ym2149/ym2149.cpp"
    "${ARES_ROOT}/ares/component/audio/ym2612/ym2612.cpp"
    "${ARES_ROOT}/ares/component/eeprom/m24c/m24c.cpp"
    "${ARES_ROOT}/ares/component/flash/sst39sf0x0/sst39sf0x0.cpp"
    "${ARES_ROOT}/thirdparty/qon/qon.cpp"
)
target_include_directories(md_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${ARES_ROOT}/ares"
)
target_link_libraries(md_core PUBLIC ares_core)
target_compile_definitions(md_core PUBLIC
    PROFILE_PERFORMANCE
    BUILD_RELEASE
    CORE_MD
)

# ---------------------------------------------------------------------------
# librashader — prebuilt Vulkan slang-shader runtime (Rust cdylib).
#
# NOT compiled here: it ships as a prebuilt .so (see
# scripts/build_librashader_android.sh) and is linked as an imported shared lib
# per ABI. libretro_emulator.so gains a NEEDED liblibrashader.so, which the
# dynamic linker resolves from the packaged jniLibs at load (minSdk 26). The
# dev-harness packages it from android/app/src/main/jniLibs; hosts receive it via
# the plugin's copy-assets hook. See .claude/findings.md → "Shaders".
# ---------------------------------------------------------------------------
add_library(librashader SHARED IMPORTED)
set_target_properties(librashader PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/liblibrashader.so"
    # Our own vendored copy of the C header, pinned to the SAME upstream tag as
    # the .so (v0.12.0) — no version skew with the ares tree's older header.
    INTERFACE_INCLUDE_DIRECTORIES "${NATIVE_ROOT}/vendor"
    # The cargo cdylib carries no DT_SONAME, so without this CMake links it by
    # full path and records that absolute BUILD path as libretro_emulator.so's
    # NEEDED entry (dlopen then fails on device). NO_SONAME links by basename, so
    # NEEDED = "liblibrashader.so", resolved from the packaged jniLibs at load.
    IMPORTED_NO_SONAME TRUE
)

# ---------------------------------------------------------------------------
# retro_emulator — JNI shared library + Vulkan renderer + pak builders
# ---------------------------------------------------------------------------
add_library(retro_emulator SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/ares_jni.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan_renderer.cpp"
    "${NATIVE_ROOT}/system_registry.cpp"
    "${NATIVE_ROOT}/save_io.cpp"
    "${NATIVE_ROOT}/pak/sfc_pak.cpp"
    "${NATIVE_ROOT}/pak/multi_pak.cpp"
    "${NATIVE_ROOT}/pak/mia_mediums.cpp"
    "${FIRMWARE_DIR}/embedded_firmware.cpp"
)
target_include_directories(retro_emulator PRIVATE
    "${ARES_ROOT}"
    "${ARES_ROOT}/ares"
    "${NATIVE_ROOT}"
    "${NATIVE_ROOT}/pak"
    "${FIRMWARE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(retro_emulator PRIVATE
    sfc_core
    fc_core
    gb_core
    md_core
    android
    log
    vulkan
    librashader
)
