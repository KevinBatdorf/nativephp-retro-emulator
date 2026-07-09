#pragma once
// iOS stub for nall/memory.hpp.
//
// nall defines jitprotect() inside #if defined(PLATFORM_MACOS), but nall
// detects both macOS and iOS as PLATFORM_MACOS (it only checks __APPLE__).
// The function calls pthread_jit_write_protect_np which the iOS SDK marks
// as __attribute__((unavailable)), causing a compile error.
//
// We temporarily clear PLATFORM_MACOS before including the real header so
// jitprotect() compiles as an empty no-op on iOS, then restore the macro.

#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
  #pragma push_macro("PLATFORM_MACOS")
  #undef PLATFORM_MACOS
  #include_next <nall/memory.hpp>
  #pragma pop_macro("PLATFORM_MACOS")
#else
  #include_next <nall/memory.hpp>
#endif
