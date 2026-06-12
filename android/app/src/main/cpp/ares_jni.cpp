#include <jni.h>
#include <android/log.h>
#include <GLES2/gl2.h>

#include <ares/ares.hpp>
#include <sfc/sfc.hpp>

#include "sfc_pak.hpp"

#define LOG_TAG "AresCore"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Emulator state — all fields are accessed exclusively from the GL thread.
// ---------------------------------------------------------------------------
struct EmulatorState {
    std::shared_ptr<vfs::directory> systemPak;
    std::shared_ptr<vfs::directory> cartridgePak;

    GLuint textureId   = 0;
    u32    frameWidth  = 0;
    u32    frameHeight = 0;

    ares::Node::System root;
    bool systemLoaded = false;
    bool romLoaded    = false;
};

static EmulatorState* g_state = nullptr;

// ---------------------------------------------------------------------------
// AndroidPlatform — wires ares callbacks to the emulator state.
// ---------------------------------------------------------------------------
struct AndroidPlatform : ares::Platform {

    auto pak(ares::Node::Object node) -> std::shared_ptr<vfs::directory> override {
        if (!g_state) return {};
        auto name = node->name();
        if (name == "Super Famicom")           return g_state->systemPak;
        if (name == "Super Famicom Cartridge") return g_state->cartridgePak;
        return {};
    }

    auto video(ares::Node::Video::Screen node,
               const u32* data, u32 pitch, u32 width, u32 height) -> void override {
        if (!g_state || !g_state->textureId) return;

        g_state->frameWidth  = width;
        g_state->frameHeight = height;

        glBindTexture(GL_TEXTURE_2D, g_state->textureId);

        // ares outputs ARGB8888 (little-endian BGRA in memory).
        // We upload with GL_RGBA so each frame the fragment shader swizzles.
        if (pitch == width) {
            glTexSubImage2D(GL_TEXTURE_2D, 0,
                            0, 0, (GLsizei)width, (GLsizei)height,
                            GL_RGBA, GL_UNSIGNED_BYTE, data);
        } else {
            for (u32 y = 0; y < height; y++) {
                glTexSubImage2D(GL_TEXTURE_2D, 0,
                                0, (GLint)y, (GLsizei)width, 1,
                                GL_RGBA, GL_UNSIGNED_BYTE, data + y * pitch);
            }
        }
    }

    auto audio(ares::Node::Audio::Stream) -> void override {}

    auto input(ares::Node::Input::Input) -> void override {}
};

static AndroidPlatform* g_platform = nullptr;

// ---------------------------------------------------------------------------
// JNI helpers
// ---------------------------------------------------------------------------
static std::vector<uint8_t> jbyteArrayToVector(JNIEnv* env, jbyteArray arr) {
    if (!arr) return {};
    jsize len = env->GetArrayLength(arr);
    std::vector<uint8_t> vec(len);
    env->GetByteArrayRegion(arr, 0, len, reinterpret_cast<jbyte*>(vec.data()));
    return vec;
}

// ---------------------------------------------------------------------------
// JNI implementations
// ---------------------------------------------------------------------------
extern "C" {

// Phase 3 — init/destroy/version -------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeInit(
    JNIEnv*, jobject)
{
    if (!g_platform) {
        g_state    = new EmulatorState{};
        g_platform = new AndroidPlatform{};
        ares::platform = g_platform;
        LOGI("ares platform initialised — version: %s", ares::Version.data());
    }
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeDestroy(
    JNIEnv*, jobject)
{
    if (g_state && g_state->systemLoaded) {
        g_state->root.reset();
    }
    ares::platform = nullptr;
    delete g_platform; g_platform = nullptr;
    delete g_state;    g_state    = nullptr;
    LOGI("ares platform destroyed");
}

JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeVersion(
    JNIEnv* env, jobject)
{
    return env->NewStringUTF(ares::Version.data());
}

// Phase 4 — GL renderer setup -----------------------------------------------

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSetupRenderer(
    JNIEnv*, jobject, jint textureId)
{
    if (!g_state) return;
    g_state->textureId = (GLuint)textureId;
    LOGI("renderer bound to texture %d", textureId);
}

// Phase 4 — system loading --------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeLoadSystem(
    JNIEnv* env, jobject,
    jbyteArray iplRomBytes, jbyteArray boardsBmlBytes)
{
    if (!g_state) return JNI_FALSE;
    if (g_state->systemLoaded) {
        LOGI("system already loaded");
        return JNI_TRUE;
    }

    auto ipl    = jbyteArrayToVector(env, iplRomBytes);
    auto boards = jbyteArrayToVector(env, boardsBmlBytes);

    if (boards.empty()) {
        LOGE("boards.bml is required for system load");
        return JNI_FALSE;
    }

    g_state->systemPak = SfcPakBuilder::makeSystemPak(
        ipl.empty() ? nullptr : ipl.data(), ipl.size(),
        boards.data(), boards.size());

    bool ok = ares::SuperFamicom::load(
        g_state->root, "[Nintendo] Super Famicom (NTSC)");

    if (!ok) {
        LOGE("ares::SuperFamicom::load failed");
        g_state->systemPak.reset();
        return JNI_FALSE;
    }

    // Wire up gamepads on both ports.
    if (auto port = g_state->root->find<ares::Node::Port>("Controller Port 1")) {
        port->allocate("Gamepad");
        port->connect();
    }
    if (auto port = g_state->root->find<ares::Node::Port>("Controller Port 2")) {
        port->allocate("Gamepad");
        port->connect();
    }

    g_state->systemLoaded = true;
    LOGI("SFC system loaded");
    return JNI_TRUE;
}

// Phase 4 — ROM loading -----------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeLoadRom(
    JNIEnv* env, jobject, jbyteArray romBytes)
{
    if (!g_state || !g_state->systemLoaded) {
        LOGE("nativeLoadRom: system not loaded");
        return JNI_FALSE;
    }

    auto rom = jbyteArrayToVector(env, romBytes);
    if (rom.empty()) {
        LOGE("nativeLoadRom: empty ROM");
        return JNI_FALSE;
    }

    auto info = SfcPakBuilder::detectHeader(rom.data(), rom.size());
    if (info.board.empty()) {
        LOGE("nativeLoadRom: could not detect ROM header");
        return JNI_FALSE;
    }
    LOGI("ROM: title='%s' board='%s' region=%s sram=%u",
         info.title.c_str(), info.board.c_str(),
         info.region.c_str(), info.sramSize);

    g_state->cartridgePak = SfcPakBuilder::makeCartridgePak(
        info, rom.data(), rom.size());

    auto cartridgeSlot =
        g_state->root->find<ares::Node::Port>("Cartridge Slot");
    if (!cartridgeSlot) {
        LOGE("nativeLoadRom: no Cartridge Slot port found");
        return JNI_FALSE;
    }

    // If a cartridge is already connected, disconnect it first.
    cartridgeSlot->disconnect();
    cartridgeSlot->allocate();
    cartridgeSlot->connect();

    g_state->root->power(false);
    g_state->romLoaded = true;
    LOGI("ROM loaded and powered on");
    return JNI_TRUE;
}

// Phase 4 — tick (real frame) -----------------------------------------------

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeTick(
    JNIEnv*, jobject)
{
    if (!g_state || !g_state->romLoaded) return;
    ares::SuperFamicom::system.run();
}

// Phase 4 — frame dimensions ------------------------------------------------

JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetFrameWidth(
    JNIEnv*, jobject)
{
    return g_state ? (jint)g_state->frameWidth : 0;
}

JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetFrameHeight(
    JNIEnv*, jobject)
{
    return g_state ? (jint)g_state->frameHeight : 0;
}

} // extern "C"
