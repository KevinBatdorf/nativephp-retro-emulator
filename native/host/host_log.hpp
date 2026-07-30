// One logging macro for host + backend code. Android routes to logcat under
// the tag the Kotlin layer filters on; everywhere else stderr.
#pragma once

#if defined(__ANDROID__)
#include <android/log.h>
#define EMUHOST_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "EmulatorCore", __VA_ARGS__)
#define EMUHOST_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "EmulatorCore", __VA_ARGS__)
#else
#include <cstdio>
#define EMUHOST_LOGI(...) (std::fprintf(stderr, __VA_ARGS__), std::fprintf(stderr, "\n"))
#define EMUHOST_LOGE(...) (std::fprintf(stderr, __VA_ARGS__), std::fprintf(stderr, "\n"))
#endif
