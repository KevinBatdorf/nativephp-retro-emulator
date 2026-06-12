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
# retro_emulator — JNI shared library + GL renderer + SFC pak builder
# ---------------------------------------------------------------------------
add_library(retro_emulator SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/ares_jni.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/sfc_pak.cpp"
)
target_include_directories(retro_emulator PRIVATE
    "${ARES_ROOT}"
    "${ARES_ROOT}/ares"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(retro_emulator PRIVATE
    sfc_core
    android
    log
    GLESv2
)
