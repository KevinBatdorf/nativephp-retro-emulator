cmake_minimum_required(VERSION 3.22)

project(retro_emulator VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Suppress C++ module scanning (not supported by NDK's CMake integration)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Android libc++ defaults to pointer-identity typeid comparison (unique RTTI,
# implementation 1). Objects cross our DSO boundary constantly (paks and node
# attributes built in libbackend_ares.so, consumed in dlopen'd cores), and each
# DSO carries its own weak typeinfo copies — pointer identity then breaks
# nall::any::is<T>() (every pak/node attribute reads back empty; the sfc base
# lost its ST-LOROM board and grew no slot ports). Implementation 2 compares
# typeinfo by name instead. Applies to every C++ TU in this project so ODR
# holds; libc++abi's __dynamic_cast is prebuilt and unaffected — that side is
# handled by native/cross_dso_vfs.hpp.
add_compile_definitions($<$<COMPILE_LANGUAGE:CXX>:_LIBCPP_TYPEINFO_COMPARISON_IMPLEMENTATION=2>)

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
            "${NATIVE_ROOT}/firmware/gba_open_bios.bin"
            "${NATIVE_ROOT}/firmware/sameboy_dmg_boot.bin"
            "${NATIVE_ROOT}/firmware/sameboy_cgb_boot.bin"
            "${NATIVE_ROOT}/firmware/sameboy_dmg_boot_fast.bin"
            "${NATIVE_ROOT}/firmware/sameboy_cgb_boot_fast.bin"
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
# ares_iface — ares/nall headers + profile defines, NO objects. Core modules
# compile against this; the runtime objects live only in libbackend_ares.so.
# ---------------------------------------------------------------------------
add_library(ares_iface INTERFACE)
target_include_directories(ares_iface INTERFACE
    "${ARES_ROOT}"
    "${ARES_ROOT}/ares"
    "${ARES_ROOT}/nall"
    "${ARES_ROOT}/thirdparty"
)
target_compile_definitions(ares_iface INTERFACE
    PROFILE_PERFORMANCE
    BUILD_RELEASE
    SLJIT_HAVE_CONFIG_PRE=1
    SLJIT_HAVE_CONFIG_POST=1
)
# Every C++ TU imports the pak-reading vfs template from libbackend_ares.so
# instead of instantiating its own — see native/cross_dso_vfs.hpp for why
# (cross-DSO dynamic_cast makes boards read empty ROMs otherwise).
target_compile_options(ares_iface INTERFACE
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${NATIVE_ROOT}/cross_dso_vfs.hpp>"
)

# ---------------------------------------------------------------------------
# ares core — the emulation framework (node tree, scheduler, platform)
# ---------------------------------------------------------------------------
add_library(ares_core OBJECT
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
target_link_libraries(sfc_core PUBLIC ares_iface)
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
target_link_libraries(fc_core PUBLIC ares_iface ymfm)
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
target_link_libraries(gb_core PUBLIC ares_iface)
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
    "${PROC}/sh2/sh2.cpp"
    "${PROC}/ssp1601/ssp1601.cpp"
)
target_include_directories(md_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${ARES_ROOT}/ares"
)
target_link_libraries(md_core PUBLIC ares_iface)
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
# the plugin's copy-assets hook.
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
# gba_core — Game Boy Advance (unity build via gba.cpp)
#   arm7tdmi — main CPU (own copy, like sfc_core's)
#   s3511a   — cartridge RTC
# ---------------------------------------------------------------------------
add_library(gba_core STATIC
    "${ARES_ROOT}/ares/gba/gba.cpp"
    "${PROC}/arm7tdmi/arm7tdmi.cpp"
    "${ARES_ROOT}/ares/component/rtc/s3511a/s3511a.cpp"
)
target_include_directories(gba_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${ARES_ROOT}/ares"
)
target_link_libraries(gba_core PUBLIC ares_iface)
target_compile_definitions(gba_core PUBLIC CORE_GBA)

# ---------------------------------------------------------------------------
# emu_host — the engine-neutral emulator host (native/host/): backend seam,
# system catalog, input/AV/state policy. Zero ares includes; compiled into
# the frontend library.
# ---------------------------------------------------------------------------
add_library(emu_host OBJECT
    "${NATIVE_ROOT}/host/backend_registry.cpp"
    "${NATIVE_ROOT}/host/system_catalog.cpp"
    "${NATIVE_ROOT}/host/emulator_host.cpp"
)
target_include_directories(emu_host PUBLIC "${NATIVE_ROOT}" "${NATIVE_ROOT}/host")
target_link_libraries(emu_host PRIVATE log)

# ---------------------------------------------------------------------------
# retro_emulator — JNI frontend + engine-neutral host + Vulkan renderer.
# Engine-free: backends arrive as libbackend_<name>.so modules that nativeInit
# dlopens (each registers itself with the host at load), and backends in turn
# load whatever engine modules they need.
# ---------------------------------------------------------------------------
add_library(retro_emulator SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/emulator_jni.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan_renderer.cpp"
)
target_include_directories(retro_emulator PRIVATE
    "${NATIVE_ROOT}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(retro_emulator PRIVATE
    emu_host
    android
    log
    vulkan
    librashader
)

# ---------------------------------------------------------------------------
# backend_ares — the ares engine behind the seam: runtime, backend adapter,
# SystemDef registry, pak/save plumbing. Exactly ONE copy per process: every
# dlopen'd core module binds ares::platform, the node system, and the
# registry from here, so a core can never fork the runtime state. Cores
# DT_NEED this library; it DT_NEEDs retro_emulator for the host symbols.
# ---------------------------------------------------------------------------
add_library(backend_ares SHARED
    "${NATIVE_ROOT}/backends/ares/ares_backend.cpp"
    "${ARES_ROOT}/ares/component/processor/z80/z80.cpp"
    "${ARES_ROOT}/ares/component/eeprom/m24c/m24c.cpp"
    "${ARES_ROOT}/ares/component/audio/sn76489/sn76489.cpp"
    "${ARES_ROOT}/ares/component/audio/ym2413/ym2413.cpp"
    "${ARES_ROOT}/ares/component/audio/ym2149/ym2149.cpp"
    "${ARES_ROOT}/ares/component/audio/ym2612/ym2612.cpp"
    "${ARES_ROOT}/thirdparty/qon/qon.cpp"
    "${ARES_ROOT}/ares/component/flash/sst39sf0x0/sst39sf0x0.cpp"
    "${NATIVE_ROOT}/system_registry.cpp"
    "${NATIVE_ROOT}/save_io.cpp"
    "${NATIVE_ROOT}/pak/multi_pak.cpp"
    "${NATIVE_ROOT}/pak/sfc_pak.cpp"
    "${NATIVE_ROOT}/pak/mia_mediums.cpp"
    "${FIRMWARE_DIR}/embedded_firmware.cpp"
)
target_include_directories(backend_ares PUBLIC
    "${ARES_ROOT}"
    "${ARES_ROOT}/ares"
    "${NATIVE_ROOT}"
    "${NATIVE_ROOT}/pak"
    "${FIRMWARE_DIR}"
)
target_link_libraries(backend_ares PUBLIC ares_core nall ares_iface)
target_link_libraries(backend_ares PRIVATE libco log retro_emulator)

# ---------------------------------------------------------------------------
# backend_sameboy — the SameBoy engine (Expat) + adapter: the bundled fast
# core for gb/gbc. Engine sources come from the pinned sameboy submodule and
# compile exactly as SameBoy's own Makefile does (GB_INTERNAL + gnu11 for the
# core; the adapter sees only the opaque API). embedded_firmware.cpp is
# compiled into this module too — each backend DSO carries its own copy
# (RTLD_LOCAL keeps them separate) so backends never link each other.
# ---------------------------------------------------------------------------
get_filename_component(SAMEBOY_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../sameboy"
    ABSOLUTE
)
# Deterministic: the submodule is pinned, so the glob is a fixed set.
file(GLOB SAMEBOY_SOURCES "${SAMEBOY_ROOT}/Core/*.c")
# SameBoy's Makefile CORE_FILTER for DISABLE_DEBUGGER: these units only
# compile with the debugger present.
list(REMOVE_ITEM SAMEBOY_SOURCES
    "${SAMEBOY_ROOT}/Core/debugger.c"
    "${SAMEBOY_ROOT}/Core/sm83_disassembler.c"
    "${SAMEBOY_ROOT}/Core/symbol_hash.c"
    "${SAMEBOY_ROOT}/Core/cheat_search.c"
)
add_library(backend_sameboy SHARED
    "${NATIVE_ROOT}/backends/sameboy/sameboy_backend.cpp"
    "${FIRMWARE_DIR}/embedded_firmware.cpp"
    ${SAMEBOY_SOURCES}
)
target_include_directories(backend_sameboy PRIVATE
    "${SAMEBOY_ROOT}"
    "${NATIVE_ROOT}"
    "${FIRMWARE_DIR}"
)
set_source_files_properties(${SAMEBOY_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-DGB_INTERNAL;-std=gnu11;-D_GNU_SOURCE;-DGB_DISABLE_DEBUGGER;-DGB_DISABLE_CHEAT_SEARCH"
)
target_compile_definitions(backend_sameboy PRIVATE GB_VERSION="1.0.3")
target_link_libraries(backend_sameboy PRIVATE retro_emulator log)

# ---------------------------------------------------------------------------
# backend_mgba — the mGBA engine (MPL-2.0) + adapter: the bundled fast core
# for gba. Built through mGBA's own CMake (libmgba only, GBA core only —
# SameBoy owns gb/gbc); optional deps that the NDK lacks stay off.
# ---------------------------------------------------------------------------
get_filename_component(MGBA_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../mgba"
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

add_library(backend_mgba SHARED
    "${NATIVE_ROOT}/backends/mgba/mgba_backend.cpp"
)
target_include_directories(backend_mgba PRIVATE
    "${NATIVE_ROOT}"
    "${MGBA_ROOT}/include"
)
# struct mCore's layout changes with mGBA's config defines (ENABLE_VFS,
# MINIMAL_CORE, ENABLE_DEBUGGERS…). The adapter must compile with exactly
# the set the library compiled with or every vtable-style call lands on the
# wrong member (SIGBUS on device).
target_compile_definitions(backend_mgba PRIVATE
    $<TARGET_PROPERTY:mgba,COMPILE_DEFINITIONS>
)
target_link_libraries(backend_mgba PRIVATE mgba retro_emulator log)

# ---------------------------------------------------------------------------
# backend_libretro — the bring-your-own-core loader. No engine ships inside:
# it dlopens libretro cores the app packages (resources/emulator-cores/) and
# adapts the libretro ABI to the seam.
# ---------------------------------------------------------------------------
add_library(backend_libretro SHARED
    "${NATIVE_ROOT}/backends/libretro/libretro_backend.cpp"
)
target_include_directories(backend_libretro PRIVATE
    "${NATIVE_ROOT}"
)
target_link_libraries(backend_libretro PRIVATE retro_emulator log)

# ---------------------------------------------------------------------------
# Per-core modules — each is its own shared library so the copy-assets hook
# can bundle only the systems a host app declares. dlopen runs the module's
# SystemRegistry::Registrar, so loading IS registering.
# ---------------------------------------------------------------------------
function(ares_core_module SYS)
    add_library(ares_core_${SYS} SHARED
        "${NATIVE_ROOT}/cores/core_${SYS}.cpp"
        ${ARGN}
    )
    target_include_directories(ares_core_${SYS} PRIVATE
        "${NATIVE_ROOT}"
        "${NATIVE_ROOT}/pak"
        "${FIRMWARE_DIR}"
    )
    target_link_libraries(ares_core_${SYS} PRIVATE ${SYS}_core backend_ares)
    set_target_properties(ares_core_${SYS} PROPERTIES OUTPUT_NAME "ares_core_${SYS}")
endfunction()

# sfc_pak.cpp lives in backend_ares with the other pak builders so every
# pak's vfs objects share one home DSO (see native/cross_dso_vfs.hpp).
ares_core_module(sfc)
ares_core_module(fc)
ares_core_module(gb)
ares_core_module(md)
ares_core_module(gba)
