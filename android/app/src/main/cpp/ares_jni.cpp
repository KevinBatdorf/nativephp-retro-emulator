#include <jni.h>
#include <android/log.h>
#include <ares/ares.hpp>

#define LOG_TAG "AresCore"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Minimal Android platform — satisfies ares::Platform virtual interface.
// Phase 4+ will replace this with real GL/audio/input implementations.
// ---------------------------------------------------------------------------
struct AndroidPlatform : ares::Platform {
    auto video(ares::Node::Video::Screen, const u32*, u32, u32, u32) -> void override {}
    auto audio(ares::Node::Audio::Stream) -> void override {}
    auto input(ares::Node::Input::Input) -> void override {}
};

static AndroidPlatform* g_platform = nullptr;

extern "C" {

// Called once per process to set up the ares platform singleton.
JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeInit(
    JNIEnv* /* env */, jobject /* obj */)
{
    if (!g_platform) {
        g_platform = new AndroidPlatform();
        ares::platform = g_platform;
        LOGI("ares platform initialised — version: %s", ares::Version.data());
    }
    return JNI_TRUE;
}

// Phase 3 stub tick — a real system is loaded in Phase 4.
// Verifies the JNI → ares call path works end-to-end without crashing.
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeTick(
    JNIEnv* /* env */, jobject /* obj */)
{
    // No system loaded yet; this is just a no-op tracer bullet.
}

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeDestroy(
    JNIEnv* /* env */, jobject /* obj */)
{
    ares::platform = nullptr;
    delete g_platform;
    g_platform = nullptr;
    LOGI("ares platform destroyed");
}

JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeVersion(
    JNIEnv* env, jobject /* obj */)
{
    return env->NewStringUTF(ares::Version.data());
}

} // extern "C"
