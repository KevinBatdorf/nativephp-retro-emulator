#include <jni.h>
#include <android/log.h>
#include <GLES2/gl2.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <ares/ares.hpp>

#include "system_registry.hpp"

#define LOG_TAG "AresCore"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Emulator state — video/ROM fields accessed from the GL thread only.
// audioMutex guards audioRingBuffer (written on GL thread, drained on audio thread).
// ---------------------------------------------------------------------------
struct EmulatorState {
    const SystemRegistry::SystemDef* system = nullptr;

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

    // Phase 7 — emulator control.
    std::atomic<bool> paused {false};

    // ROM metadata set during nativeLoadRom; read-only afterward.
    std::string romRegion;

    // Port info JSON built once during nativeLoadSystem; read-only afterward.
    std::string portsJson;
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
        if (!g_state || !g_state->system) return {};
        auto name = node->name();
        if (name == g_state->system->systemNode.c_str())    return g_state->systemPak;
        if (name == g_state->system->cartridgeNode.c_str()) return g_state->cartridgePak;
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

static std::string jstringToString(JNIEnv* env, jstring str) {
    if (!str) return {};
    const char* chars = env->GetStringUTFChars(str, nullptr);
    std::string result(chars);
    env->ReleaseStringUTFChars(str, chars);
    return result;
}

// Cache button nodes under `parent` (a controller port or, for systems with
// built-in controls, the root node) into the given port bitmask.
static void cacheButtons(ares::Node::Object parent,
                         const SystemRegistry::SystemDef& def,
                         std::atomic<uint32_t>& mask) {
    for (auto& btn : parent->find<ares::Node::Input::Button>()) {
        auto it = def.buttons.find(std::string((const char*)btn->name()));
        if (it != def.buttons.end()) {
            g_state->inputCache.push_back({btn, &mask, it->second});
        }
    }
}

static std::string buttonsJson(ares::Node::Object parent) {
    std::string json = "[";
    bool first = true;
    for (auto& btn : parent->find<ares::Node::Input::Button>()) {
        if (!first) json += ",";
        first = false;
        json += "\"";
        json += std::string((const char*)btn->name());
        json += "\"";
    }
    json += "]";
    return json;
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
    if (g_state && g_state->systemLoaded && g_state->root) {
        // unload() joins worker threads (e.g. the video screen thread) before
        // the node tree is torn down — dropping root without it lets those
        // threads race into freed state and crash.
        g_state->root->unload();
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

// Phase 4/11 — system loading ------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeLoadSystem(
    JNIEnv* env, jobject, jstring systemIdStr)
{
    if (!g_state) return JNI_FALSE;
    if (g_state->systemLoaded) {
        LOGI("system already loaded");
        return JNI_TRUE;
    }

    auto systemId = jstringToString(env, systemIdStr);
    auto* def = SystemRegistry::find(systemId);
    if (!def) {
        LOGE("unsupported system: %s", systemId.c_str());
        return JNI_FALSE;
    }

    g_state->system    = def;
    g_state->systemPak = def->makeSystemPak(*def);

    if (!def->load(g_state->root, *def)) {
        LOGE("ares %s load failed", def->id.c_str());
        g_state->systemPak.reset();
        g_state->system = nullptr;
        return JNI_FALSE;
    }

    // Wire controllers and build the button cache + ports JSON.
    std::atomic<uint32_t>* masks[2] = {
        &g_state->inputMaskPort1, &g_state->inputMaskPort2,
    };
    if (def->ports == 0) {
        // Built-in controls (e.g. Game Boy) — buttons live on the system node,
        // reported as port 1.
        cacheButtons(g_state->root, *def, g_state->inputMaskPort1);
        g_state->portsJson =
            std::string("[{\"port\":1,\"buttons\":") +
            buttonsJson(g_state->root) + "}]";
    } else {
        std::string json = "[";
        for (int i = 0; i < def->ports && i < 2; i++) {
            auto portName = std::string("Controller Port ") + std::to_string(i + 1);
            auto port = g_state->root->find<ares::Node::Port>(portName.c_str());
            if (!port) {
                LOGE("port '%s' not found", portName.c_str());
                continue;
            }
            port->allocate(def->device);
            port->connect();
            cacheButtons(port, *def, *masks[i]);
            if (json.size() > 1) json += ",";
            json += "{\"port\":" + std::to_string(i + 1) +
                    ",\"buttons\":" + buttonsJson(port) + "}";
        }
        json += "]";
        g_state->portsJson = json;
    }
    LOGI("cached %zu buttons for %s", g_state->inputCache.size(), def->id.c_str());

    g_state->systemLoaded = true;
    LOGI("%s system loaded", def->id.c_str());
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

    auto built = g_state->system->makeCartridgePak(rom.data(), rom.size());
    if (!built) {
        LOGE("nativeLoadRom: %s", built.error.c_str());
        return JNI_FALSE;
    }
    LOGI("ROM: title='%s' region='%s'", built.title.c_str(), built.region.c_str());

    g_state->cartridgePak = built.pak;

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
    g_state->romLoaded  = true;
    g_state->romRegion  = built.region;
    LOGI("ROM loaded and powered on — region=%s", built.region.c_str());
    return JNI_TRUE;
}

// Phase 4 — tick (real frame) -----------------------------------------------

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeTick(
    JNIEnv*, jobject)
{
    if (!g_state || !g_state->romLoaded) return;
    if (g_state->paused.load(std::memory_order_relaxed)) return;
    g_state->root->run();
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

// Phase 7 — pause / resume / stop ------------------------------------------

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativePause(JNIEnv*, jobject)
{
    if (g_state) g_state->paused.store(true, std::memory_order_relaxed);
    LOGI("emulator paused");
}

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeResume(JNIEnv*, jobject)
{
    if (g_state) g_state->paused.store(false, std::memory_order_relaxed);
    LOGI("emulator resumed");
}

// Phase 7 — state save / load -----------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeStateSave(
    JNIEnv* env, jobject, jstring pathStr)
{
    if (!g_state || !g_state->romLoaded) return JNI_FALSE;

    std::string pathCpp = jstringToString(env, pathStr);

    try {
        auto s = g_state->root->serialize(false);
        FILE* f = std::fopen(pathCpp.c_str(), "wb");
        if (!f) { LOGE("stateSave: cannot open %s", pathCpp.c_str()); return JNI_FALSE; }
        std::fwrite(s.data(), 1, s.size(), f);
        std::fclose(f);
        LOGI("state saved: %s (%u bytes)", pathCpp.c_str(), s.size());
        return JNI_TRUE;
    } catch (...) {
        LOGE("stateSave: exception");
        return JNI_FALSE;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeStateLoad(
    JNIEnv* env, jobject, jstring pathStr)
{
    if (!g_state || !g_state->romLoaded) return JNI_FALSE;

    std::string pathCpp = jstringToString(env, pathStr);

    FILE* f = std::fopen(pathCpp.c_str(), "rb");
    if (!f) { LOGE("stateLoad: file not found: %s", pathCpp.c_str()); return JNI_FALSE; }
    std::fseek(f, 0, SEEK_END);
    long fileSize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fileSize <= 0) { std::fclose(f); return JNI_FALSE; }

    std::vector<u8> data((size_t)fileSize);
    std::fread(data.data(), 1, (size_t)fileSize, f);
    std::fclose(f);

    try {
        nall::serializer s{data.data(), (u32)fileSize};
        bool ok = g_state->root->unserialize(s);
        LOGI("state loaded: %s — %s", pathCpp.c_str(), ok ? "ok" : "failed");
        return ok ? JNI_TRUE : JNI_FALSE;
    } catch (...) {
        LOGE("stateLoad: exception");
        return JNI_FALSE;
    }
}

// Phase 7/11 — memory read / write --------------------------------------------
// Each system exposes its work-RAM bus window (see SystemRegistry memBase/
// memSize). Must be called from the GL thread (same thread as tick) to avoid
// data races with emulation.

JNIEXPORT jbyteArray JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeReadMemory(
    JNIEnv* env, jobject, jint address, jint length)
{
    if (!g_state || !g_state->romLoaded || !g_state->system) return nullptr;
    if (length <= 0 || length > 0x10000) return nullptr;

    auto* def = g_state->system;
    uint32_t addr = (uint32_t)address;
    uint32_t len  = (uint32_t)length;

    if (addr < def->memBase) return nullptr;
    uint32_t offset = addr - def->memBase;
    if (offset + len > def->memSize) return nullptr;

    std::vector<uint8_t> bytes(len);
    for (uint32_t i = 0; i < len; i++) bytes[i] = def->memRead(offset + i);

    jbyteArray result = env->NewByteArray((jsize)len);
    env->SetByteArrayRegion(
        result, 0, (jsize)len, reinterpret_cast<const jbyte*>(bytes.data()));
    return result;
}

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeWriteMemory(
    JNIEnv* env, jobject, jint address, jbyteArray bytesArr)
{
    if (!g_state || !g_state->romLoaded || !g_state->system || !bytesArr) return;

    auto* def = g_state->system;
    uint32_t addr = (uint32_t)address;
    jsize    len  = env->GetArrayLength(bytesArr);

    if (addr < def->memBase) return;
    uint32_t offset = addr - def->memBase;
    if (offset + (uint32_t)len > def->memSize) return;

    auto bytes = jbyteArrayToVector(env, bytesArr);
    for (jsize i = 0; i < len; i++) def->memWrite(offset + i, bytes[i]);
}

// Phase 7 — region / ports --------------------------------------------------

JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetRegion(JNIEnv* env, jobject)
{
    if (!g_state) return env->NewStringUTF("");
    return env->NewStringUTF(g_state->romRegion.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetPortsJson(JNIEnv* env, jobject)
{
    if (!g_state || !g_state->systemLoaded) return env->NewStringUTF("[]");
    return env->NewStringUTF(g_state->portsJson.c_str());
}

// Phase 11 — supported systems ------------------------------------------------

/** Comma-separated ids of the systems compiled into this build (e.g. "fc,sfc,gb,md"). */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetSupportedSystems(
    JNIEnv* env, jobject)
{
    std::string ids;
    for (auto* def : SystemRegistry::all()) {
        if (!ids.empty()) ids += ",";
        ids += def->id;
    }
    return env->NewStringUTF(ids.c_str());
}

} // extern "C"
