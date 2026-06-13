#include <jni.h>
#include <android/log.h>
#include <GLES2/gl2.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <ares/ares.hpp>
#include <sfc/sfc.hpp>

#include "sfc_pak.hpp"

#define LOG_TAG "AresCore"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Emulator state — video/ROM fields accessed from the GL thread only.
// audioMutex guards audioRingBuffer (written on GL thread, drained on audio thread).
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

    // Audio — written by GL thread, read by audio drain thread.
    std::vector<ares::Node::Audio::Stream> audioStreams;
    std::mutex    audioMutex;
    std::vector<float> audioRingBuffer;
    // Cap at ~0.5 s of stereo PCM (48 kHz × 2 channels × 0.5 s).
    static constexpr size_t kAudioCap = 48000u;

    // Phase 6 — input.
    // Written by the main thread via nativeSetInputState; read by the GL thread
    // inside Platform::input(). Atomic so no mutex is needed.
    std::atomic<uint32_t> inputMaskPort1 {0};
    std::atomic<uint32_t> inputMaskPort2 {0};

    // Cached mapping from ares button node → (port mask pointer, bit).
    // Built once on the GL thread after nativeLoadSystem; read-only thereafter.
    struct CachedButton {
        ares::Node::Input::Button node;
        std::atomic<uint32_t>*   mask;
        uint32_t                 bit;
    };
    std::vector<CachedButton> inputCache;
};

static EmulatorState* g_state = nullptr;

// ---------------------------------------------------------------------------
// AndroidPlatform — wires ares callbacks to the emulator state.
// ---------------------------------------------------------------------------
struct AndroidPlatform : ares::Platform {

    auto attach(ares::Node::Object node) -> void override {
        if (!g_state) return;
        if (auto stream = node->cast<ares::Node::Audio::Stream>()) {
            stream->setResamplerFrequency(48000.0);
            g_state->audioStreams =
                g_state->root->find<ares::Node::Audio::Stream>();
            LOGI("audio stream attached — %zu total", g_state->audioStreams.size());
        }
    }

    auto detach(ares::Node::Object node) -> void override {
        if (!g_state) return;
        if (auto stream = node->cast<ares::Node::Audio::Stream>()) {
            g_state->audioStreams =
                g_state->root->find<ares::Node::Audio::Stream>();
            std::erase(g_state->audioStreams, stream);
            LOGI("audio stream detached — %zu remaining", g_state->audioStreams.size());
        }
    }

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

    auto audio(ares::Node::Audio::Stream) -> void override {
        if (!g_state || g_state->audioStreams.empty()) return;

        while (true) {
            // All streams must have at least one pending frame before we mix.
            for (auto& stream : g_state->audioStreams) {
                if (!stream->pending()) return;
            }

            f64 samples[2] = {0.0, 0.0};
            for (auto& stream : g_state->audioStreams) {
                f64 buf[2] = {0.0, 0.0};
                u32 channels = stream->read(buf);
                if (channels == 1) {
                    samples[0] += buf[0];
                    samples[1] += buf[0];
                } else {
                    samples[0] += buf[0];
                    samples[1] += buf[1];
                }
            }

            float l = (float)std::max(-1.0, std::min(+1.0, samples[0]));
            float r = (float)std::max(-1.0, std::min(+1.0, samples[1]));

            std::lock_guard<std::mutex> lock(g_state->audioMutex);
            if (g_state->audioRingBuffer.size() < EmulatorState::kAudioCap) {
                g_state->audioRingBuffer.push_back(l);
                g_state->audioRingBuffer.push_back(r);
            }
        }
    }

    auto input(ares::Node::Input::Input node) -> void override {
        if (!g_state) return;
        if (auto btn = node->cast<ares::Node::Input::Button>()) {
            for (auto& cached : g_state->inputCache) {
                if (cached.node == btn) {
                    btn->setValue(cached.mask->load(std::memory_order_relaxed) & cached.bit);
                    return;
                }
            }
        }
    }
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

    // Build button cache for both ports. Maps ares button names to bitmask
    // positions that match the Kotlin EmulatorInput constants.
    static const std::unordered_map<std::string, uint32_t> kSnesButtons = {
        {"B",      1u <<  0}, {"Y",      1u <<  1},
        {"Select", 1u <<  2}, {"Start",  1u <<  3},
        {"Up",     1u <<  4}, {"Down",   1u <<  5},
        {"Left",   1u <<  6}, {"Right",  1u <<  7},
        {"A",      1u <<  8}, {"X",      1u <<  9},
        {"L",      1u << 10}, {"R",      1u << 11},
    };
    auto cachePortButtons = [&](const char* portName, std::atomic<uint32_t>& mask) {
        auto port = g_state->root->find<ares::Node::Port>(portName);
        if (!port) { LOGE("cachePortButtons: port '%s' not found", portName); return; }
        for (auto& btn : port->find<ares::Node::Input::Button>()) {
            auto it = kSnesButtons.find(std::string((const char*)btn->name()));
            if (it != kSnesButtons.end()) {
                g_state->inputCache.push_back({btn, &mask, it->second});
            }
        }
        LOGI("cached buttons for %s (%zu total so far)", portName,
             g_state->inputCache.size());
    };
    cachePortButtons("Controller Port 1", g_state->inputMaskPort1);
    cachePortButtons("Controller Port 2", g_state->inputMaskPort2);

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

// Phase 5 — audio drain -----------------------------------------------------

/**
 * Drain mixed audio samples from the ring buffer into a caller-supplied float
 * array. Returns the number of floats written (interleaved L/R pairs).
 * Thread-safe: may be called from the audio drain thread while the GL thread
 * fills the buffer.
 */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeReadAudio(
    JNIEnv* env, jobject, jfloatArray buffer)
{
    if (!g_state) return 0;

    std::lock_guard<std::mutex> lock(g_state->audioMutex);
    if (g_state->audioRingBuffer.empty()) return 0;

    jsize capacity = env->GetArrayLength(buffer);
    jsize count    = (jsize)std::min(
        (size_t)capacity, g_state->audioRingBuffer.size());

    env->SetFloatArrayRegion(buffer, 0, count,
                             g_state->audioRingBuffer.data());

    g_state->audioRingBuffer.erase(
        g_state->audioRingBuffer.begin(),
        g_state->audioRingBuffer.begin() + count);

    return count;
}

// Phase 6 — input ---------------------------------------------------------

/**
 * Write the current button bitmask for one controller port. Safe to call from
 * any thread; the GL thread reads atomically inside Platform::input().
 *
 * @param port     1 or 2.
 * @param buttons  Bitmask — bit positions match EmulatorInput companion constants.
 */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSetInputState(
    JNIEnv*, jobject, jint port, jint buttons)
{
    if (!g_state) return;
    auto mask = static_cast<uint32_t>(buttons);
    if (port == 1) g_state->inputMaskPort1.store(mask, std::memory_order_relaxed);
    else if (port == 2) g_state->inputMaskPort2.store(mask, std::memory_order_relaxed);
}

} // extern "C"
