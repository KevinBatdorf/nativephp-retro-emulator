#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>

#include "vulkan_renderer.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <ares/ares.hpp>

#include "system_registry.hpp"
#include "save_io.hpp"
#include "cheat_parse.hpp"
#include "rate_control.hpp"
#include "core_options.hpp"

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

    u32    frameWidth  = 0;
    u32    frameHeight = 0;

    // Screen-node geometry captured alongside each frame (guarded by
    // frameMutex with it). The renderer computes presentation size from these
    // exactly like desktop-ui/program/platform.cpp:95-115.
    f64 nodeWidth   = 0.0;
    f64 nodeHeight  = 0.0;
    f64 scaleX      = 1.0;
    f64 scaleY      = 1.0;
    f64 aspectX     = 1.0;
    f64 aspectY     = 1.0;
    u32 rotation    = 0;

    // Overscan borders trimmed by default; setVideo(overscan: true) shows
    // them, mirroring desktop's Emulator::setOverscan. GL thread only.
    bool overscan = false;

    // Frame hand-off. ares' screen node delivers frames from its own worker
    // thread ("dev.ares.screen"), where no GL context is current — video()
    // buffers pixels here and the GL thread uploads on the next tick.
    std::mutex        frameMutex;
    std::vector<u32>  frameBuffer;
    bool              frameDirty = false;

    ares::Node::System root;
    bool systemLoaded = false;
    bool romLoaded    = false;

    // Audio — written by the emu thread, read by the audio drain thread.
    std::vector<ares::Node::Audio::Stream> audioStreams;
    std::mutex    audioMutex;
    std::vector<float> audioRingBuffer;
    // Cap at ~125 ms of stereo PCM (48 kHz × 2 ch × 0.125 s). Anything the
    // drain thread hasn't consumed by then is pure latency — on overflow the
    // OLDEST samples are dropped so backlog (audio lag) self-heals instead of
    // persisting for the rest of the session.
    static constexpr size_t kAudioCap = 12000u;

    // Phase 6 — input.
    // Written by the main thread via nativeSetInputState; read by the GL thread
    // inside Platform::input(). Atomic so no mutex is needed.
    std::atomic<uint32_t> inputMaskPort1 {0};
    std::atomic<uint32_t> inputMaskPort2 {0};

    // Cached mapping from ares button node → (port mask pointer, bit).
    // Built on the GL thread after nativeLoadRom. `bit` is the positional slot
    // the node reads; `defaultBit` is its unremapped value, kept so a remap can
    // be recomputed against defaults. Mutated only on the GL thread (build +
    // applyInputRemap), read on the GL thread (Platform::input) — no atomics.
    struct CachedButton {
        ares::Node::Input::Button node;
        std::atomic<uint32_t>*   mask;
        uint32_t                 bit;
        uint32_t                 defaultBit;
    };
    std::vector<CachedButton> inputCache;

    // Per-port controller remap: lowercased core-button name → the positional
    // bit that button should read (see def.buttons for the slot vocabulary).
    // Positional bits are system-independent, so a remap persists across a
    // system change and is re-applied to whatever loads. Written by the bridge
    // thread under inputRemapMutex; consumed on the GL thread when
    // inputRemapDirty is set (top of nativeTick) or right after cacheButtons.
    std::unordered_map<int, std::unordered_map<std::string, uint32_t>> inputRemap;
    std::mutex        inputRemapMutex;
    std::atomic<bool> inputRemapDirty {false};

    // Phase 7 — emulator control.
    std::atomic<bool> paused {false};

    // Phase 14 — master audio volume (0–1) and balance (−1 left … +1 right),
    // applied when mixing into the ring buffer. Atomic: set from bridge
    // threads, read on the emu thread.
    std::atomic<float> volume  {1.0f};
    std::atomic<float> balance {0.0f};

    // ROM metadata set during nativeLoadRom; read-only afterward.
    std::string romRegion;

    // Battery-save location ("<prefix>.save.ram", …). Set per nativeLoadRom;
    // empty disables persistence.
    std::string savePrefix;

    // Port info JSON built once during nativeLoadSystem; read-only afterward.
    std::string portsJson;

    // Cheats — mutated and read on the GL thread only (bridge calls arrive via
    // GLSurfaceView.queueEvent; Platform::cheat runs inside root->run()).
    // `cheats` keeps per-code pairs so removeCheat(code) works; `cheatLookup`
    // is the merged map the per-read hot path consults.
    std::map<std::string, std::map<u32, u32>> cheats;
    std::unordered_map<u32, u32> cheatLookup;

    auto rebuildCheatLookup() -> void {
        cheatLookup.clear();
        for (auto& [code, pairs] : cheats) {
            for (auto& [addr, value] : pairs) cheatLookup[addr] = value;
        }
    }

    // Rewind — desktop-ui/program/rewind.cpp semantics, run inside nativeTick.
    // GL thread only, same queueEvent routing as cheats.
    struct Rewind {
        bool enabled   = false;
        bool rewinding = false;
        u32  counter   = 0;
        u32  frequency = 10;   // capture every N frames (ares desktop default)
        u32  length    = 100;  // history cap (ares desktop default ≈ 16.7 s)
        std::vector<nall::serializer> history;
    } rewind;

    // Run-ahead — ares supports one hidden frame (desktop-ui program.cpp loop).
    // GL thread only; suppressed while fast-forwarding or rewinding, like desktop.
    bool runAheadEnabled = false;
    std::atomic<bool> fastForwardActive {false};

    // Rumble — cores publish motor state via Platform::input() on rumble
    // nodes (SFC Rumble Gamepad, GB MBC5 carts, N64 Rumble Pak). Packed
    // strong<<16|weak; the host polls per frame and drives its vibrator.
    std::atomic<bool> rumbleEnabled {false};
    std::atomic<uint32_t> rumbleState {0};

    // Dynamic rate control (see native/rate_control.hpp). Written from bridge
    // threads, read on the GL thread each tick.
    std::atomic<bool> dynamicRateControl {true};

    // True refresh rate reported by the core via Platform::refreshRateHint
    // (region- and mode-aware: SFC NTSC 60.0988, GB 59.7275, PAL ~50, …).
    // 0 until the first hint; the Kotlin pacing loop polls it per frame —
    // the analogue of ruby's Metal driver deriving its present interval
    // (ruby/video/metal/metal.cpp:118-151). Written on the emu thread.
    std::atomic<double> refreshRateHint {0.0};
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
        if (!g_state) return;

        // Runs on ares' screen worker thread — no GL context here. Buffer the
        // pixels; the GL thread uploads them in nativeTick.
        // NOTE: pitch is in BYTES (Screen::refresh passes width * sizeof(u32)).
        const u32 stride = pitch / sizeof(u32);
        std::lock_guard<std::mutex> lock(g_state->frameMutex);
        g_state->frameWidth  = width;
        g_state->frameHeight = height;
        g_state->nodeWidth   = (f64)node->width();
        g_state->nodeHeight  = (f64)node->height();
        g_state->scaleX      = node->scaleX();
        g_state->scaleY      = node->scaleY();
        g_state->aspectX     = node->aspectX();
        g_state->aspectY     = node->aspectY();
        g_state->rotation    = node->rotation();
        g_state->frameBuffer.resize((size_t)width * height);
        for (u32 y = 0; y < height; y++) {
            std::memcpy(&g_state->frameBuffer[y * width],
                        data + y * stride, width * sizeof(u32));
        }
        g_state->frameDirty = true;
    }

    auto refreshRateHint(double refreshRate) -> void override {
        if (!g_state) return;
        // Log on change like ruby's Metal driver (metal.cpp:149-151).
        if (g_state->refreshRateHint.load(std::memory_order_relaxed) != refreshRate) {
            LOGI("refresh rate hint changed to %f", refreshRate);
        }
        g_state->refreshRateHint.store(refreshRate, std::memory_order_relaxed);
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

            const float volume  = g_state->volume.load(std::memory_order_relaxed);
            const float balance = g_state->balance.load(std::memory_order_relaxed);
            l *= volume * (balance > 0.0f ? 1.0f - balance : 1.0f);
            r *= volume * (balance < 0.0f ? 1.0f + balance : 1.0f);

            std::lock_guard<std::mutex> lock(g_state->audioMutex);
            g_state->audioRingBuffer.push_back(l);
            g_state->audioRingBuffer.push_back(r);
            if (g_state->audioRingBuffer.size() > EmulatorState::kAudioCap) {
                // Drop the oldest samples — carrying them would turn a burst
                // of emulation (startup, fast-forward) into permanent lag.
                g_state->audioRingBuffer.erase(
                    g_state->audioRingBuffer.begin(),
                    g_state->audioRingBuffer.begin() +
                        (g_state->audioRingBuffer.size() - EmulatorState::kAudioCap));
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
            return;
        }
        if (auto rumble = node->cast<ares::Node::Input::Rumble>()) {
            const u32 state = g_state->rumbleEnabled.load(std::memory_order_relaxed)
                ? (u32)rumble->strongValue() << 16 | rumble->weakValue()
                : 0u;
            g_state->rumbleState.store(state, std::memory_order_relaxed);
        }
    }

    // Consulted by the cores on every CPU bus read — the empty() check keeps
    // the no-cheat hot path to a single branch. GL thread only (see
    // EmulatorState::cheats).
    auto cheat(u32 address) -> maybe<u32> override {
        if (!g_state || g_state->cheatLookup.empty()) return nothing;
        auto it = g_state->cheatLookup.find(address);
        if (it != g_state->cheatLookup.end()) return it->second;
        return nothing;
    }
};

static AndroidPlatform* g_platform = nullptr;

// The Vulkan display path (replaces the old GLES blit). Created in nativeInit
// (device/instance are window-independent); the swapchain is bound on
// nativeSurfaceCreated. All Vulkan calls happen on the render thread.
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

// Cache button nodes under `parent` (a controller port or, for systems with
// built-in controls, the root node) into the given port bitmask.
static void cacheButtons(ares::Node::Object parent,
                         const SystemRegistry::SystemDef& def,
                         std::atomic<uint32_t>& mask) {
    for (auto& btn : parent->find<ares::Node::Input::Button>()) {
        auto it = def.buttons.find(std::string((const char*)btn->name()));
        if (it != def.buttons.end()) {
            g_state->inputCache.push_back({btn, &mask, it->second, it->second});
        }
    }
}

static std::string toLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// The 1-based port a cache entry belongs to (built-in controls count as port 1).
static int portOfMask(const std::atomic<uint32_t>* mask) {
    return mask == &g_state->inputMaskPort2 ? 2 : 1;
}

// Default positional bit for a button name, case-insensitively, or false if the
// staged system has no such button.
static bool bitForButtonName(const SystemRegistry::SystemDef& def,
                             const std::string& name, uint32_t& out) {
    auto lname = toLower(name);
    for (auto& [key, bit] : def.buttons) {
        if (toLower(key) == lname) { out = bit; return true; }
    }
    return false;
}

// Recompute every cached button's read-bit from its default, overridden by the
// stored per-port remap. GL thread only (cached.bit is not atomic).
static void applyInputRemap() {
    if (!g_state) return;
    std::lock_guard<std::mutex> lock(g_state->inputRemapMutex);
    for (auto& cached : g_state->inputCache) {
        cached.bit = cached.defaultBit;
        auto pit = g_state->inputRemap.find(portOfMask(cached.mask));
        if (pit == g_state->inputRemap.end()) continue;
        auto it = pit->second.find(toLower(std::string((const char*)cached.node->name())));
        if (it != pit->second.end()) cached.bit = it->second;
    }
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
    // NB: the Vulkan renderer (g_vk) is deliberately NOT created here. Its
    // lifecycle follows the SURFACE, not the ares platform — stopEmulation()
    // and release() cycle the platform via destroy()+init(), and the swapchain
    // must survive that. g_vk is created lazily in nativeSurfaceCreated and torn
    // down only when the render thread exits (nativeReleaseRenderer).
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
    // Works around an upstream ares bug — a system loaded but never powered
    // strands dangling Thread entry points that wedge or corrupt a later
    // load. Full story on the declaration (system_registry.hpp).
    SystemRegistry::clearStaleEntryPoints();
    ares::platform = nullptr;
    // g_vk is NOT deleted here — it outlives the ares platform (see nativeInit).
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

// Phase 1.3 — Vulkan surface lifecycle + present ----------------------------
//
// The render thread drives these: surfaceCreated/Changed/Destroyed mirror the
// SurfaceHolder callbacks, presentFrame runs once per loop after the tick(s),
// setShader (un)loads a librashader preset, and screenshotRGBA reads the last
// frame back. All run on the one render thread, same as tick().

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSurfaceCreated(
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
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeReleaseRenderer(
    JNIEnv*, jobject)
{
    delete g_vk; g_vk = nullptr;
}

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSurfaceChanged(
    JNIEnv*, jobject, jint width, jint height)
{
    if (g_vk) g_vk->onResize((int)width, (int)height);
}

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSurfaceDestroyed(
    JNIEnv*, jobject)
{
    if (g_vk) g_vk->clearSurface();
}

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativePresentFrame(
    JNIEnv*, jobject, jint outX, jint outY, jint outW, jint outH)
{
    if (!g_vk || !g_state) return JNI_FALSE;
    {
        // Copy the freshest frame into the staging buffer under the frame lock;
        // the GPU work in present() then runs without holding it.
        std::lock_guard<std::mutex> lock(g_state->frameMutex);
        if (g_state->frameDirty) {
            g_vk->stageFrame(g_state->frameBuffer.data(),
                             g_state->frameWidth, g_state->frameHeight);
            g_state->frameDirty = false;
        }
    }
    return g_vk->present((int)outX, (int)outY, (int)outW, (int)outH) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSetShader(
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
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeScreenshotRGBA(
    JNIEnv* env, jobject)
{
    if (!g_vk) return nullptr;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!g_vk->screenshotRaw(rgba, w, h)) return nullptr;
    jbyteArray arr = env->NewByteArray((jsize)rgba.size());
    if (!arr) return nullptr;
    env->SetByteArrayRegion(arr, 0, (jsize)rgba.size(), reinterpret_cast<const jbyte*>(rgba.data()));
    return arr;
}

// Phase 4/11 → 4b — system staging + ROM-first boot ---------------------------
//
// No core boots without a ROM (plan 4b, agreed 2026-07-12): LoadSystem only
// STAGES the system choice, and LoadRom is the one boot path — first load and
// every swap alike — mirroring desktop, which builds a fresh system per game
// load with the region already known from the ROM analysis
// (desktop-ui/emulator/emulator.cpp:40-60, super-famicom.cpp:125-126).

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeLoadSystem(
    JNIEnv* env, jobject, jstring systemIdStr)
{
    if (!g_state) return JNI_FALSE;

    auto systemId = jstringToString(env, systemIdStr);
    auto* def = SystemRegistry::find(systemId);
    if (!def) {
        LOGE("unsupported system: %s", systemId.c_str());
        return JNI_FALSE;
    }

    // Stage only — re-staging over a running core is legal; the running game
    // continues until the next LoadRom boots the new declaration.
    g_state->system    = def;
    g_state->portsJson = SystemRegistry::staticPortsJson(*def);
    LOGI("%s system staged", def->id.c_str());
    return JNI_TRUE;
}

/**
 * Unload a running core in place, keeping g_state (texture binding, AV/pref
 * atomics) alive so the next boot inherits them. Follows the nativeDestroy
 * teardown order: flush saves, join worker threads via root->unload(), then
 * clear the per-boot state that must not leak into a fresh core.
 */
static void unloadCore()
{
    if (g_state->romLoaded && g_state->cartridgePak) {
        SaveIO::flush(g_state->cartridgePak, g_state->savePrefix);
    }
    if (g_state->root) {
        g_state->root->unload();
        g_state->root.reset();
    }
    SystemRegistry::clearStaleEntryPoints();

    g_state->systemLoaded = false;
    g_state->romLoaded    = false;
    g_state->systemPak.reset();
    g_state->cartridgePak.reset();
    g_state->inputCache.clear();
    g_state->audioStreams.clear();
    {
        std::lock_guard<std::mutex> lock(g_state->audioMutex);
        g_state->audioRingBuffer.clear();
    }
    // Game knowledge dies with the core (plan 4b): cheats, rewind timeline,
    // the stale refresh hint, and any paused flag from the old game.
    g_state->cheats.clear();
    g_state->rebuildCheatLookup();
    g_state->rewind.rewinding = false;
    g_state->rewind.counter   = 0;
    g_state->rewind.history.clear();
    g_state->refreshRateHint.store(0.0, std::memory_order_relaxed);
    g_state->paused.store(false, std::memory_order_relaxed);
}

/**
 * Returns 1 on success; 0 when the ROM was rejected BEFORE any teardown (a
 * running game is untouched); -1 when a failure after teardown began left the
 * emulator in the clean stopped state.
 */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeLoadRom(
    JNIEnv* env, jobject, jbyteArray romBytes, jstring savePrefixStr,
    jstring regionOverrideStr, jstring preferredRegionsStr)
{
    if (!g_state || !g_state->system) {
        LOGE("nativeLoadRom: no system staged");
        return 0;
    }
    auto* def = g_state->system;

    auto rom = jbyteArrayToVector(env, romBytes);
    if (rom.empty()) {
        LOGE("nativeLoadRom: empty ROM");
        return 0;
    }

    // Analyze BEFORE any teardown — a ROM that fails analysis must leave a
    // running game untouched (failures after teardown starts end in the clean
    // stopped state instead).
    auto built = def->makeCartridgePak(rom.data(), rom.size());
    if (!built) {
        LOGE("nativeLoadRom: %s", built.error.c_str());
        return 0;
    }

    auto region = SystemRegistry::resolveRegion(
        *def, built.region,
        jstringToString(env, regionOverrideStr),
        jstringToString(env, preferredRegionsStr));
    auto loadName = SystemRegistry::loadNameFor(*def, region);
    LOGI("ROM: title='%s' regions='%s' → boot '%s'",
         built.title.c_str(), built.region.c_str(), loadName.c_str());

    // Fresh core per game, like desktop.
    if (g_state->systemLoaded) unloadCore();

    g_state->systemPak = def->makeSystemPak(*def);
    if (!def->load(g_state->root, *def, loadName)) {
        LOGE("ares load failed: %s", loadName.c_str());
        unloadCore();
        return -1;
    }

    // Wire controllers and build the button cache. Ports JSON stays the
    // registry's static form — same names, same order as the node walk.
    std::atomic<uint32_t>* masks[2] = {
        &g_state->inputMaskPort1, &g_state->inputMaskPort2,
    };
    if (def->ports == 0) {
        // Built-in controls (e.g. Game Boy) — buttons live on the system node.
        cacheButtons(g_state->root, *def, g_state->inputMaskPort1);
    } else {
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
        }
    }

    // Re-apply any stored controller remap onto the fresh cache (positional
    // bits persist across boots and system changes).
    applyInputRemap();
    g_state->inputRemapDirty.store(false, std::memory_order_relaxed);

    // Desktop applies its overscan setting to every screen on load
    // (emulator.cpp:137 → setOverscan scan, emulator.cpp:242-246). Cores
    // consult screen->overscan() per frame, so this takes effect immediately.
    for (auto& screen : g_state->root->find<ares::Node::Video::Screen>()) {
        screen->setOverscan(g_state->overscan);
    }
    g_state->systemLoaded = true;

    g_state->cartridgePak = built.pak;

    // Seed battery saves from disk before the boards read the pak at connect.
    g_state->savePrefix = jstringToString(env, savePrefixStr);
    SaveIO::seed(g_state->cartridgePak, g_state->savePrefix);

    auto cartridgeSlot =
        g_state->root->find<ares::Node::Port>("Cartridge Slot");
    if (!cartridgeSlot) {
        LOGE("nativeLoadRom: no Cartridge Slot port found");
        unloadCore();
        return -1;
    }
    cartridgeSlot->allocate();
    cartridgeSlot->connect();

    g_state->root->power(false);
    g_state->romLoaded = true;
    // Report the BOOTED region — for region-free systems (gb) fall back to
    // whatever the analyzer said (usually empty).
    g_state->romRegion = region.empty() ? built.region : region;
    LOGI("ROM loaded and powered on — region=%s", g_state->romRegion.c_str());
    return 1;
}

// Phase 4 — tick (real frame) -----------------------------------------------

// Port of desktop-ui rewindRun(): in normal play, snapshot every `frequency`
// frames into a bounded ring; while rewinding, pop and restore at 5× the
// capture rate until the history is exhausted, then fall back to play.
static void rewindRun() {
    auto& rw = g_state->rewind;
    if (!rw.enabled) return;

    if (!rw.rewinding) {
        if (++rw.counter < rw.frequency) return;
        rw.counter = 0;
        if (rw.history.size() >= rw.length) rw.history.erase(rw.history.begin());
        rw.history.push_back(g_state->root->serialize(false));
        return;
    }

    if (rw.history.empty()) {
        rw.rewinding = false;
        rw.counter = 0;
        return;
    }
    if (++rw.counter < std::max(1u, rw.frequency / 5)) return;
    rw.counter = 0;
    auto s = rw.history.back();
    rw.history.pop_back();
    s.setReading();
    g_state->root->unserialize(s);
    if (rw.history.empty()) rw.rewinding = false;
}

JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeTick(
    JNIEnv*, jobject)
{
    if (!g_state || !g_state->romLoaded) return;

    // Consume a pending controller remap on the GL thread (the bridge thread
    // only stored it + flagged). Runs even while paused so it takes effect on
    // resume; the read side (Platform::input) is this same thread.
    if (g_state->inputRemapDirty.exchange(false, std::memory_order_relaxed)) {
        applyInputRemap();
    }

    if (!g_state->paused.load(std::memory_order_relaxed)) {
        // Gate DRC off during fast-forward, porting desktop's Program::event
        // FastForwardOn -> ruby::audio.setDynamic(false) (platform.cpp:29-45):
        // at 4x the resampler must not steer production toward the DAC clock.
        if (g_state->dynamicRateControl.load(std::memory_order_relaxed) &&
            !g_state->fastForwardActive.load(std::memory_order_relaxed) &&
            !g_state->audioStreams.empty()) {
            f64 fill;
            {
                std::lock_guard<std::mutex> lock(g_state->audioMutex);
                fill = (f64)g_state->audioRingBuffer.size() /
                       EmulatorState::kAudioCap;
            }
            RateControl::apply(g_state->audioStreams, fill);
        }

        rewindRun();

        // Desktop-ui run-ahead loop: run one hidden frame (video/audio
        // suppressed by the cores), snapshot, run the visible frame, roll
        // back — the visible frame is a one-frame preview that reduces
        // perceived input latency at 2× emulation cost.
        const bool runAhead = g_state->runAheadEnabled &&
            !g_state->rewind.rewinding &&
            !g_state->fastForwardActive.load(std::memory_order_relaxed);
        if (!runAhead) {
            g_state->root->run();
        } else {
            ares::setRunAhead(true);
            g_state->root->run();
            auto state = g_state->root->serialize(false);
            ares::setRunAhead(false);
            g_state->root->run();
            state.setReading();
            g_state->root->unserialize(state);
        }
    }

    // The frame produced above stays in g_state->frameBuffer (frameDirty);
    // the render thread's nativePresentFrame uploads + presents it via Vulkan.
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

/**
 * Screen-node presentation geometry for the latest frame:
 * [width, height, scaleX, scaleY, aspectX, aspectY, rotation].
 * All zeros before the first frame. Any thread (frameMutex-guarded).
 */
JNIEXPORT jdoubleArray JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetVideoGeometry(
    JNIEnv* env, jobject)
{
    jdouble values[7] = {0, 0, 1, 1, 1, 1, 0};
    if (g_state) {
        std::lock_guard<std::mutex> lock(g_state->frameMutex);
        values[0] = g_state->nodeWidth;
        values[1] = g_state->nodeHeight;
        values[2] = g_state->scaleX;
        values[3] = g_state->scaleY;
        values[4] = g_state->aspectX;
        values[5] = g_state->aspectY;
        values[6] = (jdouble)g_state->rotation;
    }
    jdoubleArray result = env->NewDoubleArray(7);
    env->SetDoubleArrayRegion(result, 0, 7, values);
    return result;
}

/**
 * True refresh rate of the loaded system as hinted by the core
 * (Platform::refreshRateHint). 0 until the first hint fires during system
 * power-on. Any thread.
 */
JNIEXPORT jdouble JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetRefreshRateHint(
    JNIEnv*, jobject)
{
    return g_state
        ? (jdouble)g_state->refreshRateHint.load(std::memory_order_relaxed)
        : 0.0;
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

/**
 * Read back the current button bitmask for one controller port — the value
 * Platform::input() will see on its next poll. Test/diagnostic seam.
 */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetInputState(
    JNIEnv*, jobject, jint port)
{
    if (!g_state) return 0;
    if (port == 1) return static_cast<jint>(g_state->inputMaskPort1.load(std::memory_order_relaxed));
    if (port == 2) return static_cast<jint>(g_state->inputMaskPort2.load(std::memory_order_relaxed));
    return 0;
}

/**
 * Merge a per-port controller remap. `emulated[i]` is a core button (as named
 * by GetPorts); `source[i]` is the positional slot that should drive it — both
 * validated against the staged system. Stores the resolved positional bits and
 * flags the GL thread to re-apply them onto the live cache on its next tick.
 * An empty pair of arrays resets the port to defaults.
 *
 * Returns "" on success, else a category-A error string for the bridge to raise
 * synchronously: "SYSTEM_NOT_LOADED", "INVALID_PARAMETERS", or
 * "UNKNOWN_BUTTON:<name>". Validation is authoritative here (native owns the
 * per-system def.buttons); the bridge thread never touches the cache directly.
 */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSetInputMapping(
    JNIEnv* env, jobject, jint port, jobjectArray emulated, jobjectArray source)
{
    auto ret = [&](const char* s) { return env->NewStringUTF(s); };
    if (!g_state || !g_state->system) return ret("SYSTEM_NOT_LOADED");
    auto& def = *g_state->system;

    // Port 1 always valid; port 2 only when the system exposes it.
    int maxPort = def.ports >= 2 ? 2 : 1;
    if (port < 1 || port > maxPort) return ret("INVALID_PARAMETERS");

    jsize count = emulated ? env->GetArrayLength(emulated) : 0;
    if (!source || env->GetArrayLength(source) != count) return ret("INVALID_PARAMETERS");

    // Resolve+validate the whole batch before mutating any state, so a bad entry
    // leaves the existing remap untouched.
    std::unordered_map<std::string, uint32_t> resolved;
    for (jsize i = 0; i < count; i++) {
        auto emu = (jstring)env->GetObjectArrayElement(emulated, i);
        auto src = (jstring)env->GetObjectArrayElement(source, i);
        auto emuName = jstringToString(env, emu);
        auto srcName = jstringToString(env, src);
        env->DeleteLocalRef(emu);
        env->DeleteLocalRef(src);

        uint32_t emuBit, srcBit;
        if (!bitForButtonName(def, emuName, emuBit))
            return ret((std::string("UNKNOWN_BUTTON:") + emuName).c_str());
        if (!bitForButtonName(def, srcName, srcBit))
            return ret((std::string("UNKNOWN_BUTTON:") + srcName).c_str());
        resolved[toLower(emuName)] = srcBit;
    }

    {
        std::lock_guard<std::mutex> lock(g_state->inputRemapMutex);
        if (count == 0) {
            g_state->inputRemap.erase(port);
        } else {
            auto& portMap = g_state->inputRemap[port];
            for (auto& [name, bit] : resolved) portMap[name] = bit;
        }
    }
    g_state->inputRemapDirty.store(true, std::memory_order_relaxed);
    return ret("");
}

/**
 * The positional bit a core button currently reads (post-remap), or -1 if the
 * staged system has no such button / nothing is loaded. Test/diagnostic seam;
 * reflects the last applied remap (call tick() after SetInputMapping to apply).
 */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetButtonBit(
    JNIEnv* env, jobject, jint port, jstring nameStr)
{
    if (!g_state) return -1;
    auto name = toLower(jstringToString(env, nameStr));
    for (auto& cached : g_state->inputCache) {
        if (portOfMask(cached.mask) != port) continue;
        if (toLower(std::string((const char*)cached.node->name())) == name)
            return static_cast<jint>(cached.bit);
    }
    return -1;
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

// Cheats ----------------------------------------------------------------------
// GL thread only: callers route through GLSurfaceView.queueEvent so the maps
// are never touched while root->run() is between reads.

/**
 * Register (or replace) a cheat under its exact code string. Returns false if
 * no valid ADDR:VALUE pair could be parsed from the code.
 */
JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeAddCheat(
    JNIEnv* env, jobject, jstring codeStr)
{
    if (!g_state) return JNI_FALSE;

    std::string code = jstringToString(env, codeStr);
    auto pairs = CheatParse::parse(code);
    if (pairs.empty()) {
        LOGE("addCheat: no valid ADDR:VALUE pairs in '%s'", code.c_str());
        return JNI_FALSE;
    }

    g_state->cheats[code] = std::move(pairs);
    g_state->rebuildCheatLookup();
    LOGI("cheat added: '%s' (%zu total)", code.c_str(), g_state->cheats.size());
    return JNI_TRUE;
}

/** Remove a cheat by its exact code string. Returns false if it wasn't active. */
JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeRemoveCheat(
    JNIEnv* env, jobject, jstring codeStr)
{
    if (!g_state) return JNI_FALSE;

    std::string code = jstringToString(env, codeStr);
    bool removed = g_state->cheats.erase(code) > 0;
    if (removed) g_state->rebuildCheatLookup();
    return removed ? JNI_TRUE : JNI_FALSE;
}

/** Deactivate all cheats. */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeClearCheats(JNIEnv*, jobject)
{
    if (!g_state) return;
    g_state->cheats.clear();
    g_state->cheatLookup.clear();
}

// Rewind / run-ahead ----------------------------------------------------------
// GL thread only (queueEvent routing): the history and flags are read inside
// nativeTick.

/**
 * Enable/disable rewind capture. bufferSeconds sizes the history at the
 * capture rate (60 fps / frequency 10 = 6 snapshots per second); <= 0 keeps
 * ares' desktop default of 100 snapshots (~16.7 s). Disabling drops history.
 */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeConfigureRewind(
    JNIEnv*, jobject, jboolean enabled, jint bufferSeconds)
{
    if (!g_state) return;
    auto& rw = g_state->rewind;
    rw.enabled = enabled;
    rw.length  = bufferSeconds > 0 ? (u32)bufferSeconds * 6u : 100u;
    if (!enabled) {
        rw.rewinding = false;
        rw.counter = 0;
        rw.history.clear();
    }
}

/**
 * Enter/exit rewind playback. Returns the new state:
 * 1 rewinding, 0 playing, -1 rewind capture is not enabled.
 */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeToggleRewind(JNIEnv*, jobject)
{
    if (!g_state || !g_state->rewind.enabled) return -1;
    auto& rw = g_state->rewind;
    rw.rewinding = !rw.rewinding;
    rw.counter = 0;
    return rw.rewinding ? 1 : 0;
}

/** Enable/disable one-frame run-ahead (see the nativeTick loop). */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSetRunAhead(
    JNIEnv*, jobject, jboolean enabled)
{
    if (g_state) g_state->runAheadEnabled = enabled;
}

/** Mirror of the Kotlin fast-forward flag — suppresses run-ahead. Any thread. */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSetFastForward(
    JNIEnv*, jobject, jboolean active)
{
    if (g_state) g_state->fastForwardActive.store(active, std::memory_order_relaxed);
}

/** Enable/disable dynamic rate control (default on). Any thread. */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSetDynamicRateControl(
    JNIEnv*, jobject, jboolean enabled)
{
    if (g_state) g_state->dynamicRateControl.store(enabled, std::memory_order_relaxed);
}

/** Gate rumble forwarding. Disabling zeroes the motor state. Any thread. */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSetRumbleEnabled(
    JNIEnv*, jobject, jboolean enabled)
{
    if (!g_state) return;
    g_state->rumbleEnabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) g_state->rumbleState.store(0, std::memory_order_relaxed);
}

/** Current motor state, packed strong<<16|weak (u16 each). Any thread. */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetRumbleState(JNIEnv*, jobject)
{
    return g_state ? (jint)g_state->rumbleState.load(std::memory_order_relaxed) : 0;
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
    // Registry data — available from staging on, no booted core required.
    if (!g_state || !g_state->system) return env->NewStringUTF("[]");
    return env->NewStringUTF(g_state->portsJson.c_str());
}

// Phase 14 — audio / video options ---------------------------------------------

/**
 * Master volume (0–1) and stereo balance (−1 left … +1 right), applied when
 * mixing into the audio ring buffer. Safe from any thread.
 */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSetAudio(
    JNIEnv*, jobject, jfloat volume, jfloat balance)
{
    if (!g_state) return;
    g_state->volume.store(std::clamp((float)volume, 0.0f, 1.0f),
                          std::memory_order_relaxed);
    g_state->balance.store(std::clamp((float)balance, -1.0f, 1.0f),
                           std::memory_order_relaxed);
}

/**
 * Video post-processing options on the ares screen node. Ranges follow ares:
 * luminance/saturation 0–1, gamma 1.0–2.0. Must run on the GL thread.
 */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSetVideo(
    JNIEnv*, jobject, jfloat luminance, jfloat saturation, jfloat gamma,
    jboolean colorBleed, jboolean overscan)
{
    if (!g_state || !g_state->systemLoaded) return;
    g_state->overscan = overscan;
    for (auto& screen : g_state->root->find<ares::Node::Video::Screen>()) {
        screen->setLuminance((f64)luminance);
        screen->setSaturation((f64)saturation);
        screen->setGamma((f64)gamma);
        screen->setColorBleed(colorBleed);
        screen->setOverscan(overscan);
    }
}

/**
 * Apply a per-system emulation toggle (Color Emulation, Deep Black Boost,
 * Interframe Blending) to the loaded core. No-ops when the core doesn't declare
 * the node, so callers apply every toggle unconditionally. GL thread only.
 */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSetCoreBoolean(
    JNIEnv* env, jobject, jstring keyStr, jboolean value)
{
    if (!g_state || !g_state->systemLoaded) return;
    CoreOptions::applyBoolean(g_state->root, jstringToString(env, keyStr), value);
}

/** Test seam: read a toggle's current node value (1/0), or -1 if absent. */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetCoreBoolean(
    JNIEnv* env, jobject, jstring keyStr)
{
    if (!g_state || !g_state->systemLoaded) return -1;
    return CoreOptions::readBoolean(g_state->root, jstringToString(env, keyStr));
}

// Phase 13 — battery-save flush ------------------------------------------------

/**
 * Write current battery-backed memory (save.ram, save.eeprom, …) to disk under
 * the prefix passed to nativeLoadRom. Must run on the GL thread (root->save()
 * touches core state). Returns false when nothing was persisted.
 */
JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeFlushSaves(
    JNIEnv*, jobject)
{
    if (!g_state || !g_state->romLoaded || g_state->savePrefix.empty()) {
        return JNI_FALSE;
    }
    // System::save() writes board memory back into the cartridge pak.
    g_state->root->save();
    return SaveIO::flush(g_state->cartridgePak, g_state->savePrefix)
        ? JNI_TRUE : JNI_FALSE;
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

/**
 * ROM file extensions valid for a system (CSV, no dots) — the LoadRom
 * family-mismatch gate, mirroring desktop's per-emulator file-dialog filters.
 * Empty string for unknown systems.
 */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetSystemExtensions(
    JNIEnv* env, jobject, jstring systemIdStr)
{
    auto* def = SystemRegistry::find(jstringToString(env, systemIdStr));
    std::string exts;
    if (def) {
        for (auto& ext : def->extensions) {
            if (!exts.empty()) exts += ",";
            exts += ext;
        }
    }
    return env->NewStringUTF(exts.c_str());
}

} // extern "C"
