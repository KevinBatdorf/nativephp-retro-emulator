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
# retro_emulator_ios — single self-contained static library for the xcframework.
#
# All object libraries are collected here so the xcframework slice is
# self-contained: the host app only needs to link -lretro_emulator_ios.
# ---------------------------------------------------------------------------
add_library(retro_emulator_ios STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/ares_ios_api.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/sfc_pak.cpp"
    $<TARGET_OBJECTS:sljit>
    $<TARGET_OBJECTS:libco>
    $<TARGET_OBJECTS:nall>
    $<TARGET_OBJECTS:ares_core>
    $<TARGET_OBJECTS:sfc_core>
)
target_include_directories(retro_emulator_ios PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ARES_ROOT}"
    "${ARES_ROOT}/ares"
)
# Propagate compile-time interface requirements from all dependencies.
target_link_libraries(retro_emulator_ios
    PRIVATE sfc_core ares_core nall libco sljit)
