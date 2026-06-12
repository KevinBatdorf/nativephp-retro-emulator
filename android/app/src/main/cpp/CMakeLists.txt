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
    # Fallback to setjmp/longjmp implementation
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
# nall's includes live one level above nall/nall/
target_include_directories(nall PUBLIC "${ARES_ROOT}/nall")
target_link_libraries(nall PUBLIC sljit)
target_compile_definitions(nall PUBLIC BUILD_RELEASE)
# Android provides pthreads in the C library — no separate link needed
target_link_libraries(nall PUBLIC log)

# ---------------------------------------------------------------------------
# ares core — the emulation framework (no system cores yet; Phase 4+)
# ---------------------------------------------------------------------------
# We compile our own ares_android.cpp instead of using configure_file on
# ares.cpp.in, because:
#   1. The resource embed (icons/logos) is not needed on Android.
#   2. We can skip the sourcery build-tool dependency entirely.
#   3. Version strings are inlined directly.
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
# retro_emulator — the JNI shared library loaded by AresCore.kt
# ---------------------------------------------------------------------------
add_library(retro_emulator SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/ares_jni.cpp"
)
target_include_directories(retro_emulator PRIVATE
    "${ARES_ROOT}"
    "${ARES_ROOT}/ares"
)
target_link_libraries(retro_emulator PRIVATE ares_core android log)
