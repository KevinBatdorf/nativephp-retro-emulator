cmake_minimum_required(VERSION 3.22)

project(retro_emulator_ios VERSION 0.1.0 LANGUAGES C CXX OBJCXX)

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
            "${CMAKE_CURRENT_SOURCE_DIR}/../native/firmware/gba_open_bios.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/../native/firmware/sameboy_dmg_boot.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/../native/firmware/sameboy_cgb_boot.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/../native/firmware/sameboy_dmg_boot_fast.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/../native/firmware/sameboy_cgb_boot_fast.bin"
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

add_library(gba_core OBJECT
    "${ARES_ROOT}/ares/gba/gba.cpp"
    "${ARES_ROOT}/ares/component/rtc/s3511a/s3511a.cpp"
)
target_include_directories(gba_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${ARES_ROOT}/ares"
)
target_link_libraries(gba_core PUBLIC ares_core)
target_compile_definitions(gba_core PUBLIC PROFILE_PERFORMANCE BUILD_RELEASE CORE_GBA)

# ---------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
# ---------------------------------------------------------------------------
# retro_emulator_ios — single self-contained static library for the xcframework.
#
# All object libraries are collected here so the xcframework slice is
# self-contained: the host app only needs to link -lretro_emulator_ios.
# ---------------------------------------------------------------------------
# SameBoy engine sources (Expat) from the pinned submodule, compiled exactly
# as SameBoy's own Makefile does; the debugger-only units are filtered the
# same way the Android module filters them.
get_filename_component(SAMEBOY_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../sameboy"
    ABSOLUTE
)
file(GLOB SAMEBOY_SOURCES "${SAMEBOY_ROOT}/Core/*.c")
list(REMOVE_ITEM SAMEBOY_SOURCES
    "${SAMEBOY_ROOT}/Core/debugger.c"
    "${SAMEBOY_ROOT}/Core/sm83_disassembler.c"
    "${SAMEBOY_ROOT}/Core/symbol_hash.c"
    "${SAMEBOY_ROOT}/Core/cheat_search.c"
)
set_source_files_properties(${SAMEBOY_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-DGB_INTERNAL;-std=gnu11;-D_GNU_SOURCE;-DGB_DISABLE_DEBUGGER;-DGB_DISABLE_CHEAT_SEARCH;-DGB_VERSION=\"1.0.3\""
)

# mGBA engine (MPL-2.0) through its own CMake — libmgba only, GBA core only.
# The adapter must compile with exactly the library's config defines or
# struct mCore's layout skews (see the Android module's note).
get_filename_component(MGBA_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../mgba"
    ABSOLUTE
)
set(LIBMGBA_ONLY ON CACHE BOOL "" FORCE)
set(M_CORE_GBA ON CACHE BOOL "" FORCE)
set(M_CORE_GB OFF CACHE BOOL "" FORCE)
set(USE_LZMA OFF CACHE BOOL "" FORCE)
set(USE_SQLITE3 OFF CACHE BOOL "" FORCE)
set(USE_PNG OFF CACHE BOOL "" FORCE)
set(USE_LIBZIP OFF CACHE BOOL "" FORCE)
set(USE_ELF OFF CACHE BOOL "" FORCE)
set(USE_DEBUGGERS OFF CACHE BOOL "" FORCE)
set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${MGBA_ROOT}" mgba EXCLUDE_FROM_ALL)

# The adapter compiles in its own object library so mGBA's config defines
# reach it (struct mCore's layout depends on them) without leaking into the
# ares/SameBoy/host translation units.
add_library(mgba_adapter OBJECT
    "${NATIVE_ROOT}/backends/mgba/mgba_backend.cpp"
)
target_include_directories(mgba_adapter PRIVATE
    "${NATIVE_ROOT}"
    "${MGBA_ROOT}/include"
)
target_compile_definitions(mgba_adapter PRIVATE
    $<TARGET_PROPERTY:mgba,COMPILE_DEFINITIONS>
)

add_library(retro_emulator_ios STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/emulator_api.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/librashader_metal_shim.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core_link.cpp"
    "${NATIVE_ROOT}/host/backend_registry.cpp"
    "${NATIVE_ROOT}/host/system_catalog.cpp"
    "${NATIVE_ROOT}/host/emulator_host.cpp"
    "${NATIVE_ROOT}/backends/ares/ares_backend.cpp"
    "${NATIVE_ROOT}/backends/sameboy/sameboy_backend.cpp"
    "${NATIVE_ROOT}/backends/libretro/libretro_backend.cpp"
    ${SAMEBOY_SOURCES}
    $<TARGET_OBJECTS:mgba_adapter>
    "${NATIVE_ROOT}/system_registry.cpp"
    "${NATIVE_ROOT}/cores/core_sfc.cpp"
    "${NATIVE_ROOT}/cores/core_fc.cpp"
    "${NATIVE_ROOT}/cores/core_gb.cpp"
    "${NATIVE_ROOT}/cores/core_md.cpp"
    "${NATIVE_ROOT}/cores/core_gba.cpp"
    "${NATIVE_ROOT}/save_io.cpp"
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
    $<TARGET_OBJECTS:gba_core>
    $<TARGET_OBJECTS:ymfm>
)
target_include_directories(retro_emulator_ios PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ARES_ROOT}"
    "${ARES_ROOT}/ares"
    "${NATIVE_ROOT}"
    "${NATIVE_ROOT}/host"
    "${NATIVE_ROOT}/pak"
    "${NATIVE_ROOT}/vendor/librashader"
    "${SAMEBOY_ROOT}"
    "${FIRMWARE_DIR}"
)

# The SameBoy adapter reports GB_VERSION; only it and the engine need the
# macro.
set_source_files_properties(
    "${NATIVE_ROOT}/backends/sameboy/sameboy_backend.cpp"
    PROPERTIES COMPILE_OPTIONS "-DGB_VERSION=\"1.0.3\""
)

target_link_libraries(retro_emulator_ios PRIVATE mgba)

# The librashader Metal shim needs ARC for the __bridge casts.
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/librashader_metal_shim.mm"
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
)
# Propagate compile-time interface requirements from all dependencies.
target_link_libraries(retro_emulator_ios
    PRIVATE sfc_core fc_core gb_core md_core gba_core ymfm ares_core nall libco sljit Threads::Threads)
