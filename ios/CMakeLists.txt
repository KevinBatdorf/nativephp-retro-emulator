cmake_minimum_required(VERSION 3.22)

project(retro_emulator_ios VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Suppress C++ module scanning (not supported by all generators).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# ares submodule root — one level above this CMakeLists.txt.
get_filename_component(ARES_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../ares"
    ABSOLUTE
)

# Shared (platform-agnostic) native sources — pak builders, system registry.
get_filename_component(NATIVE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../native"
    ABSOLUTE
)

# ---------------------------------------------------------------------------
# Embedded firmware — generated from blobs in the ares submodule
# (GB boot ROM, MD TMSS/SVP, SFC ipl.rom + boards.bml).
# ---------------------------------------------------------------------------
set(FIRMWARE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
    OUTPUT "${FIRMWARE_DIR}/embedded_firmware.cpp" "${FIRMWARE_DIR}/embedded_firmware.hpp"
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/generate_firmware.py"
            "${ARES_ROOT}" "${FIRMWARE_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/generate_firmware.py"
    COMMENT "Generating embedded firmware from ares submodule"
)


# ---------------------------------------------------------------------------
# sljit — JIT allocator used by nall.
# sljitConfigPre.h / sljitConfigPost.h live in thirdparty/ (not sljit_src/).
# ---------------------------------------------------------------------------
add_library(sljit OBJECT
    "${ARES_ROOT}/thirdparty/sljit/sljit_src/sljitLir.c"
)
target_include_directories(sljit PUBLIC "${ARES_ROOT}/thirdparty")
target_compile_definitions(sljit PUBLIC
    SLJIT_HAVE_CONFIG_PRE=1
    SLJIT_HAVE_CONFIG_POST=1
)

# ---------------------------------------------------------------------------
# libco — cooperative threading used by the ares scheduler.
# libco.c auto-includes the correct platform backend via preprocessor
# detection (__aarch64__, __x86_64__, etc.) — no explicit arch source needed.
# ---------------------------------------------------------------------------
add_library(libco OBJECT
    "${ARES_ROOT}/libco/libco.c"
)
target_include_directories(libco INTERFACE "${ARES_ROOT}")

# ---------------------------------------------------------------------------
# nall — ares support library.
# ---------------------------------------------------------------------------
add_library(nall OBJECT
    "${ARES_ROOT}/nall/nall/nall.cpp"
    "${ARES_ROOT}/nall/nall/sljitAllocator.cpp"
)
# stubs/nall/memory.hpp must come BEFORE ares/nall so our #include_next wrapper
# shadows nall's real memory.hpp and clears PLATFORM_MACOS before including it.
# This prevents the pthread_jit_write_protect_np unavailable-on-iOS compile error.
target_include_directories(nall PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${ARES_ROOT}/nall"
)
target_link_libraries(nall PUBLIC sljit)
target_compile_definitions(nall PUBLIC BUILD_RELEASE)

# ---------------------------------------------------------------------------
# ares_core — emulation framework (node tree, scheduler, platform).
# ---------------------------------------------------------------------------
add_library(ares_core OBJECT
    "${CMAKE_CURRENT_SOURCE_DIR}/ares_ios.cpp"
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
# sfc_core — Super Famicom / SNES system (unity build via sfc.cpp).
#
# stubs/ must come BEFORE ${ARES_ROOT}/ares so our resource.hpp shadows the
# missing sourcery-generated version in the ares submodule.
# ---------------------------------------------------------------------------
set(PROC "${ARES_ROOT}/ares/component/processor")
add_library(sfc_core OBJECT
    "${ARES_ROOT}/ares/sfc/sfc.cpp"
    "${PROC}/arm7tdmi/arm7tdmi.cpp"
    "${PROC}/wdc65816/wdc65816.cpp"
    "${PROC}/spc700/spc700.cpp"
    "${PROC}/gsu/gsu.cpp"
    "${PROC}/hg51b/hg51b.cpp"
    "${PROC}/upd96050/upd96050.cpp"
)
target_include_directories(sfc_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${ARES_ROOT}/ares"
)
target_link_libraries(sfc_core PUBLIC ares_core)
target_compile_definitions(sfc_core PUBLIC
    PROFILE_PERFORMANCE
    BUILD_RELEASE
    CORE_SFC
)

# ---------------------------------------------------------------------------
# ymfm — Yamaha FM synthesis library (thirdparty), used by the FC core (EPSM).
# ---------------------------------------------------------------------------
add_library(ymfm OBJECT
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
add_library(fc_core OBJECT
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
add_library(gb_core OBJECT
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
add_library(md_core OBJECT
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
# retro_emulator_ios — single self-contained static library for the xcframework.
#
# All object libraries are collected here so the xcframework slice is
# self-contained: the host app only needs to link -lretro_emulator_ios.
# ---------------------------------------------------------------------------
add_library(retro_emulator_ios STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/ares_ios_api.cpp"
    "${NATIVE_ROOT}/system_registry.cpp"
    "${NATIVE_ROOT}/pak/sfc_pak.cpp"
    "${NATIVE_ROOT}/pak/multi_pak.cpp"
    "${NATIVE_ROOT}/pak/mia_mediums.cpp"
    "${FIRMWARE_DIR}/embedded_firmware.cpp"
    $<TARGET_OBJECTS:sljit>
    $<TARGET_OBJECTS:libco>
    $<TARGET_OBJECTS:nall>
    $<TARGET_OBJECTS:ares_core>
    $<TARGET_OBJECTS:sfc_core>
    $<TARGET_OBJECTS:fc_core>
    $<TARGET_OBJECTS:gb_core>
    $<TARGET_OBJECTS:md_core>
    $<TARGET_OBJECTS:ymfm>
)
target_include_directories(retro_emulator_ios PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ARES_ROOT}"
    "${ARES_ROOT}/ares"
    "${NATIVE_ROOT}"
    "${NATIVE_ROOT}/pak"
    "${FIRMWARE_DIR}"
)
# Propagate compile-time interface requirements from all dependencies.
target_link_libraries(retro_emulator_ios
    PRIVATE sfc_core fc_core gb_core md_core ymfm ares_core nall libco sljit)
