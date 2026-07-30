// JNI marshalling over the engine-neutral EmulatorHost (native/host/).
// ares:: is deliberately absent from this file; what lives here is
// platform-shaped only — JNI type conversion, the Vulkan display path, and
// the dlopen of whichever backend modules the host app bundled.
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>

#include "vulkan_renderer.hpp"

#include "host/backend_registry.hpp"
#include "host/emulator_host.hpp"

#include <cstdio>
#include <string>
#include <vector>

#define LOG_TAG "EmulatorCore"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using EmuHost::EmulatorHost;

static EmulatorHost* g_host = nullptr;

// The Vulkan display path (replaces the old GLES blit). Created lazily in
// nativeSurfaceCreated (device/instance are window-independent); the
// swapchain is bound per surface. Its lifecycle follows the SURFACE, not the
// host — stopEmulation() and release() cycle the host via destroy()+init(),
// and the swapchain must survive that. Torn down only when the render thread
// exits (nativeReleaseRenderer). All Vulkan calls happen on the render thread.
static VulkanRenderer* g_vk = nullptr;

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

static std::string joinCsv(const std::vector<std::string>& items) {
    std::string out;
    for (auto& item : items) {
        if (!out.empty()) out += ",";
        out += item;
    }
    return out;
}

// ---------------------------------------------------------------------------
// JNI implementations
// ---------------------------------------------------------------------------
extern "C" {

// Init/destroy/version -------------------------------------------

/**
 * dlopen every bundled backend module (libbackend_<name>.so). A backend's
 * registrar runs inside dlopen, and the backend then loads whatever engine
 * modules it needs — so the set of playable systems is decided entirely by
 * what the copy-assets hook shipped, not this code. Absent modules are
 * skipped; ids no backend claims stay UNSUPPORTED_SYSTEM at the bridge.
 */
static void loadBackendModules()
{
    static bool attempted = false;
    if (attempted) return;
    attempted = true;

    static const char* kBackendNames[] = {
        "ares", "sameboy", "mgba", "libretro",
    };
    for (auto* name : kBackendNames) {
        char lib[64];
        std::snprintf(lib, sizeof(lib), "libbackend_%s.so", name);
        if (dlopen(lib, RTLD_NOW | RTLD_LOCAL)) {
            LOGI("backend module loaded: %s", lib);
        }
    }
    LOGI("registry: %zu system(s) available",
         EmuHost::Backends::availableSystems().size());
}

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeInit(
    JNIEnv*, jobject)
{
    loadBackendModules();
    if (!g_host) {
        g_host = new EmulatorHost{};
        auto* ares = EmuHost::Backends::byName("ares");
        LOGI("emulator host initialized — ares %s", ares ? ares->version() : "absent");
    }
    // NB: the Vulkan renderer (g_vk) is deliberately NOT created here — see
    // its declaration for the lifecycle story.
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeDestroy(
    JNIEnv*, jobject)
{
    // The host destructor unloads any running game (joining engine worker
    // threads) and flushes battery saves through the backend.
    delete g_host;
    g_host = nullptr;
    // g_vk is NOT deleted here — it outlives the host (see nativeInit).
    LOGI("emulator host destroyed");
}

JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeVersion(
    JNIEnv* env, jobject)
{
    auto* ares = EmuHost::Backends::byName("ares");
    return env->NewStringUTF(ares ? ares->version() : "");
}

// Vulkan surface lifecycle + present ----------------------------
//
// The render thread drives these: surfaceCreated/Changed/Destroyed mirror the
// SurfaceHolder callbacks, presentFrame runs once per loop after the tick(s),
// setShader (un)loads a librashader preset, and screenshotRGBA reads the last
// frame back. All run on the one render thread, same as tick().

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSurfaceCreated(
    JNIEnv* env, jobject, jobject surface)
{
    if (!surface) return;
    // Lazily bring up the Vulkan device the first time a surface appears; it
    // then persists across core stop/init and transient surface loss.
    if (!g_vk) {
        g_vk = new VulkanRenderer{};
        if (!g_vk->initDevice()) {
            LOGE("Vulkan device init failed — the surface will not render");
            delete g_vk; g_vk = nullptr;
            return;
        }
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) { LOGE("ANativeWindow_fromSurface returned null"); return; }
    if (!g_vk->setSurface(window)) LOGE("Vulkan setSurface failed");
}

// Final teardown of the Vulkan renderer (device + instance), called when the
// render thread exits. Transient surface loss uses nativeSurfaceDestroyed.
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeReleaseRenderer(
    JNIEnv*, jobject)
{
    delete g_vk; g_vk = nullptr;
}

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSurfaceChanged(
    JNIEnv*, jobject, jint width, jint height)
{
    if (g_vk) g_vk->onResize((int)width, (int)height);
}

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSurfaceDestroyed(
    JNIEnv*, jobject)
{
    if (g_vk) g_vk->clearSurface();
}

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativePresentFrame(
    JNIEnv*, jobject, jint outX, jint outY, jint outW, jint outH)
{
    if (!g_vk || !g_host) return JNI_FALSE;
    // Copy the freshest frame into the staging buffer under the frame lock;
    // the GPU work in present() then runs without holding it.
    g_host->withDirtyFrame([](const uint32_t* data, uint32_t w, uint32_t h) {
        g_vk->stageFrame(data, w, h);
    });
    return g_vk->present((int)outX, (int)outY, (int)outW, (int)outH) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSetShader(
    JNIEnv* env, jobject, jstring path)
{
    if (!g_vk) return JNI_FALSE;
    if (!path) return g_vk->setShader(nullptr) ? JNI_TRUE : JNI_FALSE;
    const char* p = env->GetStringUTFChars(path, nullptr);
    bool ok = g_vk->setShader(p);
    env->ReleaseStringUTFChars(path, p);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jbyteArray JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeScreenshotRGBA(
    JNIEnv* env, jobject, jintArray dims)
{
    if (!g_vk) return nullptr;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!g_vk->screenshotPresented(rgba, w, h)) return nullptr;
    if (dims && env->GetArrayLength(dims) >= 2) {
        jint wh[2] = {(jint)w, (jint)h};
        env->SetIntArrayRegion(dims, 0, 2, wh);
    }
    jbyteArray arr = env->NewByteArray((jsize)rgba.size());
    if (!arr) return nullptr;
    env->SetByteArrayRegion(arr, 0, (jsize)rgba.size(), reinterpret_cast<const jbyte*>(rgba.data()));
    return arr;
}

// System staging + ROM-first boot ---------------------------
//
// No core boots without a ROM: LoadSystem only STAGES the system choice, and
// LoadRom is the one boot path — first load and every swap alike. The host
// owns the policy (region resolution, teardown ordering, save seeding).

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeLoadSystem(
    JNIEnv* env, jobject, jstring systemIdStr, jstring biosPathStr,
    jstring backendStr)
{
    if (!g_host) return JNI_FALSE;
    return g_host->stageSystem(jstringToString(env, systemIdStr),
                               jstringToString(env, biosPathStr),
                               jstringToString(env, backendStr))
        ? JNI_TRUE : JNI_FALSE;
}

/** The engine serving calls right now: active, else staged, else "". */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetBackendName(
    JNIEnv* env, jobject)
{
    if (!g_host) return env->NewStringUTF("");
    return env->NewStringUTF(g_host->backendName().c_str());
}

/** Per-system engine availability + the fast-by-default pick, as JSON. */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetBackendsJson(
    JNIEnv* env, jobject)
{
    loadBackendModules();  // registry read — see nativeGetSupportedSystems
    return env->NewStringUTF(EmulatorHost::backendsJson().c_str());
}

// Capability gates — synchronous, so the bridge can reject an unsupported
// option loudly BEFORE queueing the (async) apply onto the emulation thread.

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeVideoSettingsSupported(
    JNIEnv*, jobject)
{
    return (g_host && g_host->videoSettingsSupported()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeToggleSupported(
    JNIEnv* env, jobject, jstring keyStr)
{
    return (g_host && g_host->toggleSupported(jstringToString(env, keyStr)))
        ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSetEngineOption(
    JNIEnv* env, jobject, jstring keyStr, jstring valueStr, jboolean staged)
{
    if (!g_host) return env->NewStringUTF("emulator not initialized");
    auto error = g_host->setEngineOption(jstringToString(env, keyStr),
                                         jstringToString(env, valueStr),
                                         staged == JNI_TRUE);
    return env->NewStringUTF(error.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetEngineOptionsJson(
    JNIEnv* env, jobject)
{
    return env->NewStringUTF(g_host ? g_host->engineOptionsJson().c_str() : "[]");
}

/**
 * Stage a Sufami Turbo slot ROM (index 0 = Slot A, 1 = Slot B) to be inserted
 * at the next nativeLoadRom, whose base must be an ST-LOROM cart. Empty bytes
 * clear the slot. Kept separate so loadRom's signature stays single-ROM.
 */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeStageSlot(
    JNIEnv* env, jobject, jint index, jbyteArray romBytes)
{
    if (!g_host) return;
    auto rom = jbyteArrayToVector(env, romBytes);
    g_host->stageSlot((int)index, rom.data(), rom.size());
}

/** Test seam: whether a staged slot cartridge actually connected at load. */
JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeIsSlotConnected(
    JNIEnv*, jobject, jint index)
{
    if (!g_host) return JNI_FALSE;
    return g_host->isSlotConnected((int)index) ? JNI_TRUE : JNI_FALSE;
}

/**
 * Stage a boot option by the engine's own option name (e.g. "Pixel Accuracy",
 * value "true"/"false"). Applied before the next boot; cores without the
 * option ignore it there.
 */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeStageBootOption(
    JNIEnv* env, jobject, jstring nameStr, jstring valueStr)
{
    if (!g_host) return;
    g_host->stageBootOption(jstringToString(env, nameStr),
                            jstringToString(env, valueStr));
}

/**
 * Read a boot option's live value from the running core — "true"/"false", or
 * "" when no core is loaded or the name is unknown. Reads engine state (e.g.
 * which SNES PPU is bound), not the staged map.
 */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetBootOption(
    JNIEnv* env, jobject, jstring nameStr)
{
    if (!g_host) return env->NewStringUTF("");
    return env->NewStringUTF(
        g_host->readBootOption(jstringToString(env, nameStr)).c_str());
}

JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeLoadRom(
    JNIEnv* env, jobject, jbyteArray romBytes, jstring savePrefixStr,
    jstring regionOverrideStr, jstring preferredRegionsStr)
{
    if (!g_host) {
        LOGE("nativeLoadRom: not initialized");
        return 0;
    }
    auto rom = jbyteArrayToVector(env, romBytes);
    return g_host->loadRom(rom.data(), rom.size(),
                           jstringToString(env, savePrefixStr),
                           jstringToString(env, regionOverrideStr),
                           jstringToString(env, preferredRegionsStr));
}

// Tick (real frame) -----------------------------------------------

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeTick(
    JNIEnv*, jobject)
{
    if (g_host) g_host->tick();
    // The produced frame stays buffered in the host (frame-dirty flag); the
    // render thread's nativePresentFrame uploads + presents it via Vulkan.
}

// Frame dimensions ------------------------------------------------

JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetFrameWidth(
    JNIEnv*, jobject)
{
    return g_host ? (jint)g_host->frameWidth() : 0;
}

JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetFrameHeight(
    JNIEnv*, jobject)
{
    return g_host ? (jint)g_host->frameHeight() : 0;
}

/**
 * Screen-node presentation geometry for the latest frame:
 * [width, height, scaleX, scaleY, aspectX, aspectY, rotation].
 * All zeros before the first frame. Any thread (frame-lock guarded).
 */
JNIEXPORT jdoubleArray JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetVideoGeometry(
    JNIEnv* env, jobject)
{
    jdouble values[7] = {0, 0, 1, 1, 1, 1, 0};
    if (g_host) g_host->videoGeometry(values);
    jdoubleArray result = env->NewDoubleArray(7);
    env->SetDoubleArrayRegion(result, 0, 7, values);
    return result;
}

/**
 * True refresh rate of the loaded system as hinted by the engine. 0 until the
 * first hint fires during system power-on. Any thread.
 */
JNIEXPORT jdouble JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetRefreshRateHint(
    JNIEnv*, jobject)
{
    return g_host ? (jdouble)g_host->refreshRateHint() : 0.0;
}

// Audio drain -----------------------------------------------------

/**
 * Drain mixed audio samples from the ring buffer into a caller-supplied float
 * array. Returns the number of floats written (interleaved L/R pairs).
 * Thread-safe against the emulation thread filling the ring.
 */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeReadAudio(
    JNIEnv* env, jobject, jfloatArray buffer)
{
    if (!g_host) return 0;
    jsize capacity = env->GetArrayLength(buffer);
    if (capacity <= 0) return 0;
    std::vector<float> tmp((size_t)capacity);
    size_t count = g_host->readAudio(tmp.data(), (size_t)capacity);
    if (count == 0) return 0;
    env->SetFloatArrayRegion(buffer, 0, (jsize)count, tmp.data());
    return (jint)count;
}

// Input ---------------------------------------------------------

/**
 * Write the current hardware button bitmask for one controller port. Safe
 * from any thread; the emulation thread samples atomically at poll time.
 */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSetInputState(
    JNIEnv*, jobject, jint port, jint buttons)
{
    if (g_host) g_host->setInput((int)port, (uint32_t)buttons);
}

/**
 * Read back the combined (hardware | software) button bitmask for one port —
 * the value the engine will see on its next input poll. Test seam.
 */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetInputState(
    JNIEnv*, jobject, jint port)
{
    return g_host ? (jint)g_host->combinedInput((int)port) : 0;
}

/**
 * Names of the buttons currently held on a port, comma-joined, resolved
 * against the port's connected device. Empty when nothing is held.
 */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetPressedButtons(
    JNIEnv* env, jobject, jint port)
{
    if (!g_host) return env->NewStringUTF("");
    return env->NewStringUTF(g_host->pressedButtons((int)port).c_str());
}

/**
 * Merge a per-port controller remap; the host validates wholesale against the
 * staged system and applies on the emulation thread. "" success or a
 * category-A error string for the bridge to raise synchronously.
 */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSetInputMapping(
    JNIEnv* env, jobject, jint port, jobjectArray emulated, jobjectArray source)
{
    // Staged-system gate precedes shape validation: callers key on
    // SYSTEM_NOT_LOADED to distinguish "nothing to remap against" from a
    // malformed pair list.
    if (!g_host || !g_host->systemStaged()) return env->NewStringUTF("SYSTEM_NOT_LOADED");

    jsize count = emulated ? env->GetArrayLength(emulated) : 0;
    if (!source || env->GetArrayLength(source) != count)
        return env->NewStringUTF("INVALID_PARAMETERS");

    std::vector<std::string> emulatedNames, sourceNames;
    emulatedNames.reserve((size_t)count);
    sourceNames.reserve((size_t)count);
    for (jsize i = 0; i < count; i++) {
        auto emu = (jstring)env->GetObjectArrayElement(emulated, i);
        auto src = (jstring)env->GetObjectArrayElement(source, i);
        emulatedNames.push_back(jstringToString(env, emu));
        sourceNames.push_back(jstringToString(env, src));
        env->DeleteLocalRef(emu);
        env->DeleteLocalRef(src);
    }
    return env->NewStringUTF(
        g_host->setInputMapping((int)port, emulatedNames, sourceNames).c_str());
}

/**
 * The positional bit a core button currently reads (post-remap), or -1 when
 * nothing is bound for (port, name). Test seam; call tick() after
 * SetInputMapping to apply.
 */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetButtonBit(
    JNIEnv* env, jobject, jint port, jstring nameStr)
{
    if (!g_host) return -1;
    return (jint)g_host->getButtonBit((int)port, jstringToString(env, nameStr));
}

/** The pending (unconsumed) accumulated delta on one axis. Test seam. */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetAxisAccum(
    JNIEnv* env, jobject, jint port, jstring nameStr)
{
    if (!g_host) return 0;
    return (jint)g_host->getAxisAccum((int)port, jstringToString(env, nameStr));
}

/**
 * Register (or swap) the device on a port. Validated synchronously against
 * the catalog; the engine rebind is deferred to the emulation thread. An
 * empty name disconnects. Registrations persist across loadRom.
 */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeConnectDevice(
    JNIEnv* env, jobject, jstring systemIdStr, jint port, jstring deviceStr)
{
    if (!g_host) return env->NewStringUTF("SYSTEM_NOT_LOADED");
    return env->NewStringUTF(
        g_host->connectDevice(jstringToString(env, systemIdStr), (int)port,
                              jstringToString(env, deviceStr)).c_str());
}

/**
 * The LOGICAL port(s) a physical port's registered device occupies — one for
 * a normal controller, four for a Super Multitap. Empty if the system isn't
 * available.
 */
JNIEXPORT jintArray JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeDevicePorts(
    JNIEnv* env, jobject, jstring systemIdStr, jint physical)
{
    std::vector<int> ports;
    if (g_host) {
        ports = g_host->devicePorts(jstringToString(env, systemIdStr), (int)physical);
    }
    std::vector<jint> out(ports.begin(), ports.end());
    jintArray arr = env->NewIntArray((jsize)out.size());
    if (!out.empty()) env->SetIntArrayRegion(arr, 0, (jsize)out.size(), out.data());
    return arr;
}

/**
 * Set or clear one software button on a port, resolved against the connected
 * device's own button set. Software bits merge with the hardware mask.
 */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativePressButton(
    JNIEnv* env, jobject, jint port, jstring nameStr, jboolean down)
{
    if (!g_host) return env->NewStringUTF("SYSTEM_NOT_LOADED");
    return env->NewStringUTF(
        g_host->pressButton((int)port, jstringToString(env, nameStr),
                            down == JNI_TRUE).c_str());
}

/**
 * Accumulate a relative delta on one axis of the connected device (mouse /
 * light-gun X/Y). Consumed on the engine's next poll.
 */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSetAxis(
    JNIEnv* env, jobject, jint port, jstring nameStr, jint value)
{
    if (!g_host) return env->NewStringUTF("SYSTEM_NOT_LOADED");
    return env->NewStringUTF(
        g_host->setAxis((int)port, jstringToString(env, nameStr), (int)value).c_str());
}

/**
 * Aim a light-gun at an absolute normalized screen position (0..1) — the
 * host's shadow cursor feeds the relative delta the engine's gun needs.
 */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeAimAt(
    JNIEnv* env, jobject, jint port, jfloat nx, jfloat ny)
{
    if (!g_host) return env->NewStringUTF("SYSTEM_NOT_LOADED");
    return env->NewStringUTF(g_host->aimAt((int)port, (float)nx, (float)ny).c_str());
}

// Pause / resume / stop ------------------------------------------

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativePause(JNIEnv*, jobject)
{
    if (g_host) g_host->pause();
    LOGI("emulator paused");
}

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeResume(JNIEnv*, jobject)
{
    if (g_host) g_host->resume();
    LOGI("emulator resumed");
}

// State save / load -----------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeStateSave(
    JNIEnv* env, jobject, jstring pathStr)
{
    if (!g_host) return JNI_FALSE;
    return g_host->stateSave(jstringToString(env, pathStr)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeStateLoad(
    JNIEnv* env, jobject, jstring pathStr)
{
    if (!g_host) return JNI_FALSE;
    return g_host->stateLoad(jstringToString(env, pathStr)) ? JNI_TRUE : JNI_FALSE;
}

// Memory read / write --------------------------------------------
// Each system exposes its work-RAM bus window (see the system catalog).
// Must be called from the GL thread (same thread as tick) to avoid data
// races with emulation.

JNIEXPORT jbyteArray JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeReadMemory(
    JNIEnv* env, jobject, jint address, jint length)
{
    if (!g_host) return nullptr;
    // JNI-allocation guard predating the seam; the iOS bridge has no such
    // cap, so it lives here rather than in the host.
    if (length <= 0 || length > 0x10000) return nullptr;

    std::vector<uint8_t> bytes((size_t)length);
    int written = g_host->readMemory((uint32_t)address, bytes.data(), (uint32_t)length);
    if (written < 0) return nullptr;

    jbyteArray result = env->NewByteArray((jsize)written);
    env->SetByteArrayRegion(
        result, 0, (jsize)written, reinterpret_cast<const jbyte*>(bytes.data()));
    return result;
}

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeWriteMemory(
    JNIEnv* env, jobject, jint address, jbyteArray bytesArr)
{
    if (!g_host || !bytesArr) return;
    auto bytes = jbyteArrayToVector(env, bytesArr);
    g_host->writeMemory((uint32_t)address, bytes.data(), (uint32_t)bytes.size());
}

// Cheats ----------------------------------------------------------------------
// GL thread only: callers route through the render thread's queueEvent so the
// maps are never touched while the engine is between reads.

/**
 * Register (or replace) a cheat under its exact code string. Returns false if
 * no valid ADDR:VALUE pair could be parsed from the code.
 */
JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeAddCheat(
    JNIEnv* env, jobject, jstring codeStr)
{
    if (!g_host) return JNI_FALSE;
    return g_host->addCheat(jstringToString(env, codeStr)) ? JNI_TRUE : JNI_FALSE;
}

/** Remove a cheat by its exact code string. Returns false if it wasn't active. */
JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeRemoveCheat(
    JNIEnv* env, jobject, jstring codeStr)
{
    if (!g_host) return JNI_FALSE;
    return g_host->removeCheat(jstringToString(env, codeStr)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeClearCheats(JNIEnv*, jobject)
{
    if (g_host) g_host->clearCheats();
}

// Rewind / run-ahead ----------------------------------------------------------
// GL thread only (queueEvent routing): the history and flags are read inside
// nativeTick.

/**
 * Enable/disable rewind capture. bufferSeconds sizes the history at the
 * capture rate (6 snapshots per second); <= 0 keeps the default of 100
 * snapshots (~16.7 s). Disabling drops history.
 */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeConfigureRewind(
    JNIEnv*, jobject, jboolean enabled, jint bufferSeconds)
{
    if (g_host) g_host->configureRewind(enabled == JNI_TRUE, (int)bufferSeconds);
}

/**
 * Enter/exit rewind playback. Returns the new state:
 * 1 rewinding, 0 playing, -1 rewind capture is not enabled.
 */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeToggleRewind(JNIEnv*, jobject)
{
    return g_host ? (jint)g_host->toggleRewind() : -1;
}

/** Enable/disable one-frame run-ahead (see the host tick loop). */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSetRunAhead(
    JNIEnv*, jobject, jboolean enabled)
{
    if (g_host) g_host->setRunAhead(enabled == JNI_TRUE);
}

/** Mirror of the Kotlin fast-forward flag — suppresses run-ahead. Any thread. */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSetFastForward(
    JNIEnv*, jobject, jboolean active)
{
    if (g_host) g_host->setFastForward(active == JNI_TRUE);
}

/** Enable/disable dynamic rate control (default on). Any thread. */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSetDynamicRateControl(
    JNIEnv*, jobject, jboolean enabled)
{
    if (g_host) g_host->setDynamicRateControl(enabled == JNI_TRUE);
}

/** Gate rumble forwarding. Disabling zeroes the motor state. Any thread. */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSetRumbleEnabled(
    JNIEnv*, jobject, jboolean enabled)
{
    if (g_host) g_host->setRumbleEnabled(enabled == JNI_TRUE);
}

/** Current motor state, packed strong<<16|weak (u16 each). Any thread. */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetRumbleState(JNIEnv*, jobject)
{
    return g_host ? (jint)g_host->rumbleState() : 0;
}

// Region / ports --------------------------------------------------

JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetRegion(JNIEnv* env, jobject)
{
    if (!g_host) return env->NewStringUTF("");
    return env->NewStringUTF(g_host->region().c_str());
}

JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetPortsJson(JNIEnv* env, jobject)
{
    // Available from staging on, no booted core required.
    if (!g_host) return env->NewStringUTF("[]");
    return env->NewStringUTF(g_host->portsJson().c_str());
}

// Audio / video options ---------------------------------------------

/**
 * Master volume (0–1) and stereo balance (−1 left … +1 right), applied when
 * mixing into the audio ring buffer. Safe from any thread.
 */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSetAudio(
    JNIEnv*, jobject, jfloat volume, jfloat balance)
{
    if (g_host) g_host->setAudio((float)volume, (float)balance);
}

/**
 * Video post-processing options on the engine's screens. Ranges follow the
 * engine: luminance/saturation 0–1, gamma 1.0–2.0. Must run on the GL thread.
 */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSetVideo(
    JNIEnv*, jobject, jfloat luminance, jfloat saturation, jfloat gamma,
    jboolean colorBleed, jboolean overscan)
{
    if (g_host) g_host->setVideo((float)luminance, (float)saturation, (float)gamma,
                                 colorBleed == JNI_TRUE, overscan == JNI_TRUE);
}

/**
 * Apply a per-system emulation toggle (Color Emulation, Deep Black Boost,
 * Interframe Blending) to the loaded core. No-ops when the core doesn't
 * declare the node, so callers apply every toggle unconditionally. GL thread.
 */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeSetCoreBoolean(
    JNIEnv* env, jobject, jstring keyStr, jboolean value)
{
    if (g_host) g_host->setCoreBoolean(jstringToString(env, keyStr), value == JNI_TRUE);
}

/** Test seam: read a toggle's current node value (1/0), or -1 if absent. */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetCoreBoolean(
    JNIEnv* env, jobject, jstring keyStr)
{
    if (!g_host) return -1;
    return (jint)g_host->coreBoolean(jstringToString(env, keyStr));
}

// Battery-save flush ------------------------------------------------

/**
 * Write current battery-backed memory (save.ram, save.eeprom, …) to disk
 * under the prefix passed to nativeLoadRom. Must run on the GL thread.
 * Returns false when nothing was persisted.
 */
JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeFlushSaves(
    JNIEnv*, jobject)
{
    if (!g_host) return JNI_FALSE;
    return g_host->flushSaves() ? JNI_TRUE : JNI_FALSE;
}

// Supported systems ------------------------------------------------

/** Comma-separated ids of the systems available in this build (e.g. "fc,gb,gba,gbc,md,sfc"). */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetSupportedSystems(
    JNIEnv* env, jobject)
{
    // Registry reads must not depend on a surface existing: nativeInit only
    // runs when a renderer boots, but GetSystems is called from plain screens
    // (a console list) long before any surface. Idempotent.
    loadBackendModules();
    return env->NewStringUTF(joinCsv(EmuHost::Backends::availableSystems()).c_str());
}

/**
 * ROM file extensions valid for a system (CSV, no dots) — the LoadRom
 * family-mismatch gate. Empty string for unavailable systems.
 */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_EmulatorCore_nativeGetSystemExtensions(
    JNIEnv* env, jobject, jstring systemIdStr)
{
    loadBackendModules();  // registry read — see nativeGetSupportedSystems
    return env->NewStringUTF(joinCsv(
        EmulatorHost::systemExtensionsFor(jstringToString(env, systemIdStr))).c_str());
}

} // extern "C"
