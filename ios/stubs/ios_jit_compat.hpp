#pragma once
// Force-included into every C/C++ translation unit for iOS builds.
//
// nall's memory.hpp calls pthread_jit_write_protect_np() inside a
// #if defined(PLATFORM_MACOS) block, but nall detects both macOS AND iOS as
// PLATFORM_MACOS (it only checks __APPLE__). On iOS the SDK marks the
// function __attribute__((unavailable)), causing a compile error.
//
// We macro-replace the call before pthread.h has a chance to declare it with
// the unavailability attribute, so the compiler never sees the raw call.
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define pthread_jit_write_protect_np(x) do { (void)(x); } while(0)
#endif
