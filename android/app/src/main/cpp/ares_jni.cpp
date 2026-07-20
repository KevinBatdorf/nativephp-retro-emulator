#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>

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
#include "node_util.hpp"
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

    // Slotted media (SuFami Turbo slots A/B). Slot ROM bytes are staged by the
    // bridge before loadRom; the pak is built + connected during the boot and
    // handed to the "Sufami Turbo Cartridge" nodes by Platform::pak (parent port
    // name disambiguates A from B). GL-thread only, like the other paks.
    std::vector<uint8_t>            stagedSlot[2];
    std::shared_ptr<vfs::directory> slotPak[2];
    bool                            slotConnected[2] = {false, false};  // test seam

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

    // Phase 6 — input. Two per-port button masks OR'd together at poll time:
    //   hwMask — hardware gamepad bits, written by the UI thread via
    //            nativeSetInputState (keycode/motion → positional bits).
    //   swMask — software bits, written natively by pressButton/setButtons with
    //            the connected device's own bit for the named button.
    // Ports are 1-based; index [0]=port1 … up to kMaxPorts (multitap territory).
    static constexpr int kMaxPorts = 5;
    std::atomic<uint32_t> hwMask[kMaxPorts] {};
    std::atomic<uint32_t> swMask[kMaxPorts] {};

    // Cached mapping from an ares button node → (1-based port, bit). Built on the
    // GL thread when a device is (re)connected. `bit` is the slot the node reads;
    // `defaultBit` is its unremapped value, kept so a remap recomputes against
    // defaults. Mutated only on the GL thread (build + applyInputRemap), read on
    // the GL thread (Platform::input) — no atomics needed on the entries.
    struct CachedButton {
        ares::Node::Input::Button node;
        int      port;
        uint32_t bit;
        uint32_t defaultBit;
    };
    std::vector<CachedButton> inputCache;

    // Cached axis nodes (mouse / light-gun X/Y). Each frame the core polls an
    // axis; we hand it the accumulated relative delta for (port, name) and reset
    // it to zero (accumulate-and-consume — matches ares' relative-motion model).
    struct CachedAxis {
        ares::Node::Input::Axis node;
        int         port;
        std::string name;
    };
    std::vector<CachedAxis> axisCache;
    std::mutex axisMutex;   // guards axisAccum + lightgun cursor
    std::unordered_map<std::string, int32_t> axisAccum[kMaxPorts];

    // Light-gun shadow cursor per port, mirroring ares' internal cursor (starts
    // center-screen, same clamp as super-scope.cpp:52-56) so aimAt() can feed the
    // relative delta to reach an absolute normalized position. Reset on connect.
    static constexpr int32_t kGunW = 256, kGunH = 240;
    int32_t lightgunX[kMaxPorts];
    int32_t lightgunY[kMaxPorts];

    // Explicit device registration by PHYSICAL port (index 0 = port 1, only the
    // system's real ports). "" = nothing plugged in. Persisted across boots.
    // A Super Multitap here fans out to several LOGICAL ports; the logical→device
    // mapping is derived on demand (see connectedDescriptor). Guarded by
    // deviceMutex.
    std::string       connectedDevice[kMaxPorts];
    std::mutex        deviceMutex;
    std::atomic<bool> deviceDirty {false};

    // Per-port controller remap: lowercased core-button name → the bit that
    // button should read. Written by the bridge thread under inputRemapMutex;
    // consumed on the GL thread when inputRemapDirty is set (top of nativeTick)
    // or right after a device (re)connect.
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

    // Dev-supplied firmware image (LoadSystem biosPath), staged with the
    // system; handed to makeSystemPak on every boot. Empty = none given.
    std::vector<uint8_t> biosBytes;

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

    auto status(string_view message) -> void override {
        LOGI("ares status: %.*s", (int)message.size(), message.data());
    }

    auto attach(ares::Node::Object node) -> void override {
        if (!g_state) return;
        if (auto stream = NodeUtil::as<ares::Node::Audio::Stream>(node)) {
            stream->setResamplerFrequency(48000.0);
            g_state->audioStreams =
                NodeUtil::findAll<ares::Node::Audio::Stream>(g_state->root);
            LOGI("audio stream attached — %zu total", g_state->audioStreams.size());
        }
    }

    auto detach(ares::Node::Object node) -> void override {
        if (!g_state) return;
        if (auto stream = NodeUtil::as<ares::Node::Audio::Stream>(node)) {
            g_state->audioStreams =
                NodeUtil::findAll<ares::Node::Audio::Stream>(g_state->root);
            std::erase(g_state->audioStreams, stream);
            LOGI("audio stream detached — %zu remaining", g_state->audioStreams.size());
        }
    }

    auto pak(ares::Node::Object node) -> std::shared_ptr<vfs::directory> override {
        if (!g_state || !g_state->system) return {};
        // The root IS the system node — match by identity, not by name.
        if (node == g_state->root) return g_state->systemPak;
        auto name = node->name();
        if (name == g_state->system->cartridgeNode.c_str()) return g_state->cartridgePak;
        // Sufami Turbo slot carts — same node name in both slots, so the parent
        // port ("Sufami Turbo Slot A" / "…B") picks which staged pak answers.
        if (name == "Sufami Turbo Cartridge") {
            auto parent = ares::Node::parent(node);
            std::string port = parent ? std::string((const char*)parent->name()) : "";
            if (port == "Sufami Turbo Slot A") return g_state->slotPak[0];
            if (port == "Sufami Turbo Slot B") return g_state->slotPak[1];
        }
        if (name == "BS Memory Cartridge") return g_state->slotPak[0];
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
        if (auto btn = NodeUtil::as<ares::Node::Input::Button>(node)) {
            for (auto& cached : g_state->inputCache) {
                if (cached.node == btn) {
                    int i = cached.port - 1;
                    uint32_t mask = g_state->hwMask[i].load(std::memory_order_relaxed)
                                  | g_state->swMask[i].load(std::memory_order_relaxed);
                    btn->setValue(mask & cached.bit);
                    return;
                }
            }
            return;
        }
        if (auto axis = NodeUtil::as<ares::Node::Input::Axis>(node)) {
            for (auto& cached : g_state->axisCache) {
                if (cached.node == axis) {
                    std::lock_guard<std::mutex> lock(g_state->axisMutex);
                    auto& acc = g_state->axisAccum[cached.port - 1][cached.name];
                    axis->setValue(acc);
                    acc = 0;   // consume: a relative delta applies once per poll
                    return;
                }
            }
            axis->setValue(0);
            return;
        }
        if (auto rumble = NodeUtil::as<ares::Node::Input::Rumble>(node)) {
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

static std::string toLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// A connectable device's inputs: button node name → bit, plus axis node names.
struct DeviceDescriptor {
    std::unordered_map<std::string, uint32_t> buttons;
    std::vector<std::string> axes;
};

// Non-default-gamepad devices per system. The default gamepad (def.device) is
// described by def.buttons in the registry; everything else lives here so the
// registry (and its button drift test) stays gamepad-only. SNES-first: Mouse
// now; light-guns / multitap land next.
static const std::unordered_map<std::string,
       std::unordered_map<std::string, DeviceDescriptor>>& deviceTable() {
    static const std::unordered_map<std::string,
           std::unordered_map<std::string, DeviceDescriptor>> t = {
        {"sfc", {
            {"Mouse", {{{"Left", 1u << 0}, {"Right", 1u << 1}}, {"X", "Y"}}},
            {"Super Scope", {{{"Trigger", 1u << 0}, {"Cursor", 1u << 1},
                              {"Turbo", 1u << 2}, {"Pause", 1u << 3}}, {"X", "Y"}}},
            {"Justifier", {{{"Trigger", 1u << 0}, {"Start", 1u << 3}}, {"X", "Y"}}},
            // Container device — no inputs of its own; fans out to gamepad
            // sub-ports (handled specially in applyConnectedDevices).
            {"Super Multitap", {{}, {}}},
        }},
    };
    return t;
}

// Resolve a device name to its descriptor for a system, or false if unsupported.
static bool resolveDevice(const SystemRegistry::SystemDef& def,
                          const std::string& name, DeviceDescriptor& out) {
    if (def.device && name == def.device) {          // the system's default pad
        out.buttons = def.buttons;
        out.axes.clear();
        return true;
    }
    auto sit = deviceTable().find(def.id);
    if (sit != deviceTable().end()) {
        auto dit = sit->second.find(name);
        if (dit != sit->second.end()) { out = dit->second; return true; }
    }
    return false;
}

// Device names this system accepts: the default gamepad + any table extras.
static std::vector<std::string> supportedDevices(const SystemRegistry::SystemDef& def) {
    std::vector<std::string> v;
    if (def.device) v.emplace_back(def.device);
    auto sit = deviceTable().find(def.id);
    if (sit != deviceTable().end())
        for (auto& [n, _] : sit->second) v.push_back(n);
    return v;
}

// Multitap container devices — ares peripheral name + logical fan-out width.
struct MultitapInfo { const char* name; int block; };
static const MultitapInfo* multitapInfo(const std::string& systemId) {
    static const std::unordered_map<std::string, MultitapInfo> t = {
        {"sfc", {"Super Multitap", 4}},
    };
    auto it = t.find(systemId);
    return it == t.end() ? nullptr : &it->second;
}
static bool isMultitap(const std::string& name) {
    if (!g_state || !g_state->system) return false;
    auto* m = multitapInfo(g_state->system->id);
    return m && name == m->name;
}
// Logical ports a physical port consumes: the multitap's width, else 1.
static int portBlock(const std::string& name) {
    if (!isMultitap(name)) return 1;
    return multitapInfo(g_state->system->id)->block;
}

// Cache a device's button + axis nodes under `parent` for `port` (1-based).
static void cacheDevice(ares::Node::Object parent,
                        const DeviceDescriptor& desc, int port) {
    for (auto& btn : NodeUtil::findAll<ares::Node::Input::Button>(parent)) {
        auto it = desc.buttons.find(std::string((const char*)btn->name()));
        if (it != desc.buttons.end())
            g_state->inputCache.push_back({btn, port, it->second, it->second});
    }
    for (auto& ax : NodeUtil::findAll<ares::Node::Input::Axis>(parent)) {
        auto name = std::string((const char*)ax->name());
        if (std::find(desc.axes.begin(), desc.axes.end(), name) != desc.axes.end())
            g_state->axisCache.push_back({ax, port, name});
    }
}

// Bit for a button name in a descriptor, case-insensitively; false if absent.
static bool bitForButtonName(const DeviceDescriptor& desc,
                             const std::string& name, uint32_t& out) {
    auto lname = toLower(name);
    for (auto& [key, bit] : desc.buttons)
        if (toLower(key) == lname) { out = bit; return true; }
    return false;
}

// Recompute every cached button's read-bit from its default, overridden by the
// stored per-port remap. GL thread only (cached.bit is not atomic).
static void applyInputRemap() {
    if (!g_state) return;
    std::lock_guard<std::mutex> lock(g_state->inputRemapMutex);
    for (auto& cached : g_state->inputCache) {
        cached.bit = cached.defaultBit;
        auto pit = g_state->inputRemap.find(cached.port);
        if (pit == g_state->inputRemap.end()) continue;
        auto it = pit->second.find(toLower(std::string((const char*)cached.node->name())));
        if (it != pit->second.end()) cached.bit = it->second;
    }
}

// Rebuild the input/axis caches to match connectedDevice[] on the live core.
// GL thread only. Systems with built-in controls (ports == 0, e.g. Game Boy)
// always cache their controls on the system node. Otherwise every registered
// device is (re)allocated on its hot-swappable port; empty ports get nothing.
static void applyConnectedDevices() {
    if (!g_state || !g_state->system || !g_state->root) return;
    auto& def = *g_state->system;
    g_state->inputCache.clear();
    g_state->axisCache.clear();

    if (def.ports == 0) {   // built-in controls (Game Boy) — always on logical 1
        DeviceDescriptor desc; desc.buttons = def.buttons;
        cacheDevice(g_state->root, desc, 1);
        applyInputRemap();
        return;
    }

    // Walk physical ports, expanding a multitap into 4 gamepad sub-ports, and
    // assign consecutive LOGICAL port numbers (a multitap on port 2 → 2,3,4,5).
    int logical = 1;
    for (int p = 1; p <= def.ports && logical <= EmulatorState::kMaxPorts; p++) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(g_state->deviceMutex);
            name = g_state->connectedDevice[p - 1];
        }
        auto portName = std::string("Controller Port ") + std::to_string(p);
        auto port = NodeUtil::findByName<ares::Node::Port>(g_state->root, portName.c_str());

        if (name.empty()) { if (port) port->disconnect(); logical += 1; continue; }
        if (!port) { logical += portBlock(name); continue; }

        if (isMultitap(name)) {
            port->allocate(name.c_str());
            port->connect();
            auto tap = NodeUtil::connected(port);   // the multitap peripheral
            int block = portBlock(name);
            DeviceDescriptor gp; gp.buttons = def.buttons;   // each sub-port is a gamepad
            for (int i = 1; i <= block && logical <= EmulatorState::kMaxPorts; i++, logical++) {
                auto sub = tap ? NodeUtil::findByName<ares::Node::Port>(tap, (std::string("Controller Port ") + std::to_string(i)).c_str()) : nullptr;
                if (!sub) continue;
                sub->allocate("Gamepad");
                sub->connect();
                cacheDevice(sub, gp, logical);
            }
        } else {
            DeviceDescriptor desc;
            if (resolveDevice(def, name, desc)) {
                port->allocate(name.c_str());
                port->connect();
                cacheDevice(port, desc, logical);
            }
            logical += 1;
        }
    }

    applyInputRemap();
}

// ---------------------------------------------------------------------------
// JNI implementations
// ---------------------------------------------------------------------------
extern "C" {

// Phase 3 — init/destroy/version -------------------------------------------

/**
 * dlopen every bundled core module. Each module's SystemRegistry::Registrar
 * runs inside dlopen, so the registry ends up holding exactly the systems the
 * host app shipped — the copy-assets hook decides that set, not this code.
 * Absent modules are skipped; ids the registry never sees stay
 * UNSUPPORTED_SYSTEM at the bridge.
 */
static void loadCoreModules()
{
    static bool attempted = false;
    if (attempted) return;
    attempted = true;

    static const char* kCoreIds[] = {
        "fc", "sfc", "gb", "gba", "md", "n64",
    };
    for (auto* id : kCoreIds) {
        char name[64];
        std::snprintf(name, sizeof(name), "libretro_core_%s.so", id);
        if (dlopen(name, RTLD_NOW | RTLD_LOCAL)) {
            LOGI("core module loaded: %s", name);
        }
    }
    LOGI("registry: %zu system(s) compiled in", SystemRegistry::all().size());
}

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeInit(
    JNIEnv*, jobject)
{
    loadCoreModules();
    if (!g_platform) {
        g_state    = new EmulatorState{};
        g_platform = new AndroidPlatform{};
        ares::platform = g_platform;
        LOGI("ares platform initialized — version: %s", ares::Version.data());
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

// Phase 4/11 → 4b — system staging + ROM-first boot ---------------------------
//
// No core boots without a ROM (plan 4b, agreed 2026-07-12): LoadSystem only
// STAGES the system choice, and LoadRom is the one boot path — first load and
// every swap alike — mirroring desktop, which builds a fresh system per game
// load with the region already known from the ROM analysis
// (desktop-ui/emulator/emulator.cpp:40-60, super-famicom.cpp:125-126).

JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeLoadSystem(
    JNIEnv* env, jobject, jstring systemIdStr, jstring biosPathStr)
{
    if (!g_state) return JNI_FALSE;

    auto systemId = jstringToString(env, systemIdStr);
    auto* def = SystemRegistry::find(systemId);
    if (!def) {
        LOGE("unsupported system: %s", systemId.c_str());
        return JNI_FALSE;
    }

    // Adreno's shader compiler rejects parallel-RDP's specialized per-combiner
    // compute shaders (vkCreateComputePipelines → VK_ERROR_UNKNOWN), but it
    // compiles the single ubershader fine — force that path. Read by
    // parallel-RDP at RDP init (loadRom). TODO: gate on driver_id == Qualcomm
    // so faster GPUs keep the specialized shaders.
    if (systemId == "n64") {
        setenv("PARALLEL_RDP_UBERSHADER", "1", 1);
    }

    // An optional dev-supplied BIOS travels with the staging (gba may override
    // its embedded open BIOS with a real dump); empty when none was given.
    g_state->biosBytes.clear();
    auto biosPath = jstringToString(env, biosPathStr);
    if (!biosPath.empty()) {
        auto file = nall::file::read(nall::string(biosPath.c_str()));
        if (!file.empty()) {
            g_state->biosBytes.assign(file.begin(), file.end());
            LOGI("bios staged: %s (%zu bytes)", biosPath.c_str(), g_state->biosBytes.size());
        } else {
            LOGE("bios not readable: %s", biosPath.c_str());
        }
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
    g_state->slotPak[0].reset();
    g_state->slotPak[1].reset();
    g_state->slotConnected[0] = g_state->slotConnected[1] = false;
    g_state->inputCache.clear();
    g_state->axisCache.clear();
    // connectedDevice[] intentionally survives (registrations persist across a
    // reboot); the swMask does not — a device teardown drops held buttons.
    for (int i = 0; i < EmulatorState::kMaxPorts; i++) {
        g_state->swMask[i].store(0, std::memory_order_relaxed);
    }
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
 * Stage a Sufami Turbo slot ROM (index 0 = Slot A, 1 = Slot B) to be inserted at
 * the next nativeLoadRom, whose base must be an ST-LOROM cart. Empty bytes clear
 * the slot. Kept separate so loadRom's signature stays single-ROM.
 */
JNIEXPORT void JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeStageSlot(
    JNIEnv* env, jobject, jint index, jbyteArray romBytes)
{
    if (!g_state || index < 0 || index > 1) return;
    g_state->stagedSlot[index] = jbyteArrayToVector(env, romBytes);
}

/** Test seam: whether a Sufami Turbo slot cartridge actually connected at load. */
JNIEXPORT jboolean JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeIsSlotConnected(
    JNIEnv*, jobject, jint index)
{
    if (!g_state || index < 0 || index > 1) return JNI_FALSE;
    return g_state->slotConnected[index] ? JNI_TRUE : JNI_FALSE;
}

/**
 * Shared boot: takes an already-built game pak (cartridge bytes or disc
 * media — analysis happened in the caller, BEFORE any teardown, so a bad
 * file leaves a running game untouched) and runs the fresh-core boot.
 * Returns 1 on success; 0 pre-teardown rejection; -1 when a failure after
 * teardown began left the emulator in the clean stopped state; -2 when the
 * system requires firmware and none was staged.
 */
static int bootWithPak(SystemRegistry::CartridgePak built,
                       const std::string& savePrefix,
                       const std::string& regionOverride,
                       const std::string& preferredRegions)
{
    auto* def = g_state->system;

    auto region = SystemRegistry::resolveRegion(
        *def, built.region, regionOverride, preferredRegions);
    auto loadName = SystemRegistry::loadNameFor(*def, region);
    LOGI("ROM: title='%s' regions='%s' → boot '%s'",
         built.title.c_str(), built.region.c_str(), loadName.c_str());

    // Fresh core per game, like desktop.
    if (g_state->systemLoaded) unloadCore();

    g_state->systemPak = def->makeSystemPak(*def, g_state->biosBytes);
    if (!def->load(g_state->root, *def, loadName)) {
        LOGE("ares load failed: %s", loadName.c_str());
        unloadCore();
        return -1;
    }

    // Controllers are registered explicitly (connectDevice), not auto-allocated:
    // re-connect whatever the dev registered before this boot (persists across
    // loadRom) and build its caches. A system with no registrations boots with
    // no input; built-in-controls systems (ports == 0) always cache their node.
    applyConnectedDevices();
    g_state->inputRemapDirty.store(false, std::memory_order_relaxed);
    g_state->deviceDirty.store(false, std::memory_order_relaxed);

    // Desktop applies its overscan setting to every screen on load
    // (emulator.cpp:137 → setOverscan scan, emulator.cpp:242-246). Cores
    // consult screen->overscan() per frame, so this takes effect immediately.
    for (auto& screen : NodeUtil::findAll<ares::Node::Video::Screen>(g_state->root)) {
        screen->setOverscan(g_state->overscan);
    }
    g_state->systemLoaded = true;

    g_state->cartridgePak = built.pak;

    // Seed battery saves from disk before the boards read the pak at connect.
    g_state->savePrefix = savePrefix;
    SaveIO::seed(g_state->cartridgePak, g_state->savePrefix);

    auto cartridgeSlot =
        NodeUtil::findByName<ares::Node::Port>(g_state->root, "Cartridge Slot");
    if (!cartridgeSlot) {
        LOGE("loadRom: no Cartridge Slot port found");
        unloadCore();
        return -1;
    }
    ares::Node::Peripheral baseCartridge = cartridgeSlot->allocate();
    cartridgeSlot->connect();

    // Slotted media: connecting an ST-LOROM base created the Sufami Turbo slot
    // ports UNDER the base cartridge peripheral (not the root), so find them
    // there. Build a pak from each staged slot ROM and connect it (Platform::pak
    // hands it to the slot's cartridge node). Do this before power-on so the game
    // is present at boot rather than the base's insert-cartridge menu.
    // Two shapes: SuFami Turbo (two slots) or BS-X (one BS Memory slot). Pick by
    // which port the base created; stagedSlot[0] feeds the single BS slot.
    bool isBsx = baseCartridge && (bool)NodeUtil::findByName<ares::Node::Port>(baseCartridge, "BS Memory Slot");
    struct SlotDef { const char* port; bool flash; };
    SlotDef slots[2];
    int slotCount;
    if (isBsx) {
        slots[0] = {"BS Memory Slot", true};
        slotCount = 1;
    } else {
        slots[0] = {"Sufami Turbo Slot A", false};
        slots[1] = {"Sufami Turbo Slot B", false};
        slotCount = 2;
    }
    for (int i = 0; i < slotCount; i++) {
        if (g_state->stagedSlot[i].empty()) continue;
        if (!def->makeSlotPak) { LOGE("system '%s' has no slot pak builder", def->id.c_str()); continue; }
        auto slot = baseCartridge ? NodeUtil::findByName<ares::Node::Port>(baseCartridge, slots[i].port)
                                   : ares::Node::Port();
        if (!slot) { LOGE("slot port '%s' not found", slots[i].port); continue; }
        g_state->slotPak[i] = def->makeSlotPak(
            i, slots[i].flash, g_state->stagedSlot[i].data(), g_state->stagedSlot[i].size());
        slot->allocate();
        slot->connect();
        g_state->slotConnected[i] = (bool)NodeUtil::connected(slot);
        LOGI("slot '%s' connected=%d (%zu bytes)",
             slots[i].port, g_state->slotConnected[i] ? 1 : 0, g_state->stagedSlot[i].size());
        g_state->stagedSlot[i].clear();   // pak copied the bytes
    }

    g_state->root->power(false);
    g_state->romLoaded = true;
    // Report the BOOTED region — for region-free systems (gb) fall back to
    // whatever the analyzer said (usually empty).
    g_state->romRegion = region.empty() ? built.region : region;
    LOGI("ROM loaded and powered on — region=%s", g_state->romRegion.c_str());
    return 1;
}

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

    return bootWithPak(std::move(built),
                       jstringToString(env, savePrefixStr),
                       jstringToString(env, regionOverrideStr),
                       jstringToString(env, preferredRegionsStr));
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

    // Consume pending controller changes on the GL thread (the bridge thread
    // only stored them + flagged). A device (re)connect rebuilds the caches
    // (which re-applies the remap); a lone remap just recomputes bits. Runs even
    // while paused; the read side (Platform::input) is this same thread.
    if (g_state->deviceDirty.exchange(false, std::memory_order_relaxed)) {
        applyConnectedDevices();
        g_state->inputRemapDirty.store(false, std::memory_order_relaxed);
    } else if (g_state->inputRemapDirty.exchange(false, std::memory_order_relaxed)) {
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
    if (!g_state || port < 1 || port > EmulatorState::kMaxPorts) return;
    g_state->hwMask[port - 1].store(static_cast<uint32_t>(buttons), std::memory_order_relaxed);
}

/**
 * Read back the combined (hardware | software) button bitmask for one port —
 * the value Platform::input() will see on its next poll. Test/diagnostic seam.
 */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetInputState(
    JNIEnv*, jobject, jint port)
{
    if (!g_state || port < 1 || port > EmulatorState::kMaxPorts) return 0;
    int i = port - 1;
    return static_cast<jint>(g_state->hwMask[i].load(std::memory_order_relaxed)
                           | g_state->swMask[i].load(std::memory_order_relaxed));
}

// Defined below with the other input helpers; used by the remap validation.
static bool connectedDescriptor(int port, DeviceDescriptor& out);

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

    if (port < 1 || port > EmulatorState::kMaxPorts) return ret("INVALID_PARAMETERS");

    // Remap names belong to the device at this logical port (multitap-aware).
    DeviceDescriptor desc;
    if (!connectedDescriptor(port, desc))
        return ret("INVALID_PARAMETERS"); // no controller registered on this port

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
        if (!bitForButtonName(desc, emuName, emuBit))
            return ret((std::string("UNKNOWN_BUTTON:") + emuName).c_str());
        if (!bitForButtonName(desc, srcName, srcBit))
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
        if (cached.port != port) continue;
        if (toLower(std::string((const char*)cached.node->name())) == name)
            return static_cast<jint>(cached.bit);
    }
    return -1;
}

/**
 * The pending (unconsumed) accumulated delta on one axis. Test/diagnostic seam;
 * a poll (see Platform::input) drains it to 0, so read it before ticking.
 */
JNIEXPORT jint JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetAxisAccum(
    JNIEnv* env, jobject, jint port, jstring nameStr)
{
    if (!g_state || port < 1 || port > EmulatorState::kMaxPorts) return 0;
    auto name = jstringToString(env, nameStr);
    std::lock_guard<std::mutex> lock(g_state->axisMutex);
    auto& m = g_state->axisAccum[port - 1];
    auto it = m.find(name);
    return it == m.end() ? 0 : static_cast<jint>(it->second);
}

// Resolve the descriptor of the device at a LOGICAL port (a multitap sub-port
// reads "Gamepad"), or the built-in controls for a ports==0 system. Computed
// on-demand from the synchronously-set connectedDevice[] so it never races the
// deferred allocate. Returns false if nothing is connected there.
static bool connectedDescriptor(int port, DeviceDescriptor& out) {
    if (!g_state || !g_state->system) return false;
    auto& def = *g_state->system;
    if (def.ports == 0) { out.buttons = def.buttons; out.axes.clear(); return true; }
    std::string name;
    {
        std::lock_guard<std::mutex> lock(g_state->deviceMutex);
        int logical = 1;
        for (int p = 1; p <= def.ports && logical <= EmulatorState::kMaxPorts; p++) {
            auto& dev = g_state->connectedDevice[p - 1];
            int block = portBlock(dev);
            for (int i = 0; i < block && logical <= EmulatorState::kMaxPorts; i++, logical++) {
                if (logical == port) { name = isMultitap(dev) ? std::string("Gamepad") : dev; }
            }
            if (!name.empty()) break;
        }
    }
    if (name.empty()) return false;
    return resolveDevice(def, name, out);
}

/**
 * Register (or swap) the device on a port. Validates against the staged system
 * synchronously; the actual allocate/connect + cache rebuild happens on the GL
 * thread (deviceDirty → nativeTick). An empty name disconnects the port. The
 * registration persists across loadRom. Returns "" or a category-A error.
 */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeConnectDevice(
    JNIEnv* env, jobject, jstring systemIdStr, jint port, jstring deviceStr)
{
    auto ret = [&](const char* s) { return env->NewStringUTF(s); };
    if (!g_state) return ret("SYSTEM_NOT_LOADED");

    // Validate against the staged system by id (from the registry, static) so we
    // don't race the asynchronous LoadSystem staging on the render thread —
    // g_state->system may not be set yet when this bridge call lands.
    auto* def = SystemRegistry::find(jstringToString(env, systemIdStr));
    if (!def) return ret("SYSTEM_NOT_LOADED");

    // Built-in-controls systems (Game Boy) have no ports to plug into — the
    // controls are always present, so a connect is a harmless no-op.
    if (def->ports == 0) return ret("");

    int maxPort = std::min(def->ports, EmulatorState::kMaxPorts);
    if (port < 1 || port > maxPort) return ret("INVALID_PARAMETERS");

    auto name = jstringToString(env, deviceStr);
    DeviceDescriptor desc;
    if (!name.empty() && !resolveDevice(*def, name, desc)) return ret("UNSUPPORTED_DEVICE");

    {
        std::lock_guard<std::mutex> lock(g_state->deviceMutex);
        g_state->connectedDevice[port - 1] = name;
    }
    {
        // Reset the light-gun shadow cursor to center, matching ares' fresh
        // device (super-scope.hpp init). Telescoping deltas keep it in sync even
        // if aimAt runs before the deferred allocate.
        std::lock_guard<std::mutex> lock(g_state->axisMutex);
        g_state->lightgunX[port - 1] = EmulatorState::kGunW / 2;
        g_state->lightgunY[port - 1] = EmulatorState::kGunH / 2;
    }
    g_state->deviceDirty.store(true, std::memory_order_relaxed);
    return ret("");
}

/**
 * The LOGICAL port(s) a physical port's registered device occupies — one for a
 * normal controller, four for a Super Multitap (its four players). Computed from
 * the current registrations so the bridge can hand back one Controller per
 * logical port. Empty if the system isn't found.
 */
JNIEXPORT jintArray JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeDevicePorts(
    JNIEnv* env, jobject, jstring systemIdStr, jint physical)
{
    std::vector<jint> out;
    auto* def = SystemRegistry::find(jstringToString(env, systemIdStr));
    if (def) {
        int logical = 1;
        for (int p = 1; p <= def->ports && logical <= EmulatorState::kMaxPorts; p++) {
            std::string name;
            {
                std::lock_guard<std::mutex> lock(g_state->deviceMutex);
                name = g_state->connectedDevice[p - 1];
            }
            int block = portBlock(name);
            for (int i = 0; i < block && logical <= EmulatorState::kMaxPorts; i++, logical++) {
                if (p == physical) out.push_back(logical);
            }
        }
    }
    jintArray arr = env->NewIntArray((jsize)out.size());
    if (!out.empty()) env->SetIntArrayRegion(arr, 0, (jsize)out.size(), out.data());
    return arr;
}

/**
 * Set or clear one software button on a port, resolved against the connected
 * device's own button set. Software bits merge with the hardware mask. Returns
 * "" / "SYSTEM_NOT_LOADED" / "UNKNOWN_BUTTON:<name>".
 */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativePressButton(
    JNIEnv* env, jobject, jint port, jstring nameStr, jboolean down)
{
    auto ret = [&](const char* s) { return env->NewStringUTF(s); };
    if (!g_state || !g_state->system) return ret("SYSTEM_NOT_LOADED");
    if (port < 1 || port > EmulatorState::kMaxPorts) return ret("INVALID_PARAMETERS");

    DeviceDescriptor desc;
    auto name = jstringToString(env, nameStr);
    uint32_t bit;
    if (!connectedDescriptor(port, desc) || !bitForButtonName(desc, name, bit))
        return ret((std::string("UNKNOWN_BUTTON:") + name).c_str());

    if (down) g_state->swMask[port - 1].fetch_or(bit, std::memory_order_relaxed);
    else      g_state->swMask[port - 1].fetch_and(~bit, std::memory_order_relaxed);
    return ret("");
}

/**
 * Accumulate a relative delta on one axis of the connected device (mouse /
 * light-gun X/Y). The value is consumed on the next poll (see Platform::input).
 * Returns "" / "SYSTEM_NOT_LOADED" / "INVALID_PARAMETERS" (unknown axis).
 */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeSetAxis(
    JNIEnv* env, jobject, jint port, jstring nameStr, jint value)
{
    auto ret = [&](const char* s) { return env->NewStringUTF(s); };
    if (!g_state || !g_state->system) return ret("SYSTEM_NOT_LOADED");
    if (port < 1 || port > EmulatorState::kMaxPorts) return ret("INVALID_PARAMETERS");

    DeviceDescriptor desc;
    auto name = jstringToString(env, nameStr);
    if (!connectedDescriptor(port, desc) ||
        std::find(desc.axes.begin(), desc.axes.end(), name) == desc.axes.end())
        return ret("INVALID_PARAMETERS");

    std::lock_guard<std::mutex> lock(g_state->axisMutex);
    g_state->axisAccum[port - 1][name] += value;
    return ret("");
}

/**
 * Aim a light-gun at an absolute normalized screen position (0..1). ares' guns
 * are relative-only (they accumulate deltas into an internal cursor), so we track
 * a shadow cursor mirroring that cursor and feed the delta needed to reach the
 * target. Requires the connected device to expose X and Y axes. Returns "" /
 * "SYSTEM_NOT_LOADED" / "INVALID_PARAMETERS" (no pointing device on the port).
 */
JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeAimAt(
    JNIEnv* env, jobject, jint port, jfloat nx, jfloat ny)
{
    auto ret = [&](const char* s) { return env->NewStringUTF(s); };
    if (!g_state || !g_state->system) return ret("SYSTEM_NOT_LOADED");
    if (port < 1 || port > EmulatorState::kMaxPorts) return ret("INVALID_PARAMETERS");

    DeviceDescriptor desc;
    auto has = [&](const char* a) {
        return std::find(desc.axes.begin(), desc.axes.end(), a) != desc.axes.end();
    };
    if (!connectedDescriptor(port, desc) || !has("X") || !has("Y"))
        return ret("INVALID_PARAMETERS");

    float cx = nx < 0 ? 0 : (nx > 1 ? 1 : nx);
    float cy = ny < 0 ? 0 : (ny > 1 ? 1 : ny);
    int tx = (int)(cx * EmulatorState::kGunW);
    int ty = (int)(cy * EmulatorState::kGunH);

    std::lock_guard<std::mutex> lock(g_state->axisMutex);
    g_state->axisAccum[port - 1]["X"] += tx - g_state->lightgunX[port - 1];
    g_state->axisAccum[port - 1]["Y"] += ty - g_state->lightgunY[port - 1];
    g_state->lightgunX[port - 1] = tx;   // target is in-bounds (0..W/0..H)
    g_state->lightgunY[port - 1] = ty;
    return ret("");
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

static std::string jsonStringArray(const std::vector<std::string>& items) {
    std::string s = "[";
    for (auto& it : items) { if (s.size() > 1) s += ","; s += "\"" + it + "\""; }
    return s + "]";
}

// Button names of a descriptor, ordered by bit (stable output).
static std::vector<std::string> orderedButtons(const DeviceDescriptor& desc) {
    std::vector<std::pair<uint32_t, std::string>> ordered;
    for (auto& [name, bit] : desc.buttons) ordered.push_back({bit, name});
    std::sort(ordered.begin(), ordered.end());
    std::vector<std::string> names;
    for (auto& [bit, name] : ordered) names.push_back(name);
    return names;
}

// Ports with the currently-connected device, its inputs, and what each port
// supports. Built from registry + registrations — no booted core required.
static std::string buildPortsJson() {
    auto& def = *g_state->system;
    auto supported = jsonStringArray(supportedDevices(def));

    if (def.ports == 0) {   // built-in controls — always present, no registration
        DeviceDescriptor desc; desc.buttons = def.buttons;
        return "[{\"port\":1,\"device\":null,\"buttons\":"
             + jsonStringArray(orderedButtons(desc))
             + ",\"axes\":[],\"supported\":" + supported + "}]";
    }

    std::string json = "[";
    auto emit = [&](int lport, const std::string& devName, const DeviceDescriptor* d) {
        if (json.size() > 1) json += ",";
        json += "{\"port\":" + std::to_string(lport)
              + ",\"device\":" + (devName.empty() ? "null" : "\"" + devName + "\"")
              + ",\"buttons\":" + (d ? jsonStringArray(orderedButtons(*d)) : "[]")
              + ",\"axes\":" + (d ? jsonStringArray(d->axes) : "[]")
              + ",\"supported\":" + supported + "}";
    };

    int logical = 1;
    for (int p = 1; p <= def.ports && logical <= EmulatorState::kMaxPorts; p++) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(g_state->deviceMutex);
            name = g_state->connectedDevice[p - 1];
        }
        if (isMultitap(name)) {   // fans out to gamepad logical ports
            DeviceDescriptor gp; gp.buttons = def.buttons;
            int block = portBlock(name);
            for (int i = 0; i < block && logical <= EmulatorState::kMaxPorts; i++, logical++)
                emit(logical, "Gamepad", &gp);
        } else {
            DeviceDescriptor desc;
            bool ok = !name.empty() && resolveDevice(def, name, desc);
            emit(logical, ok ? name : std::string(), ok ? &desc : nullptr);
            logical++;
        }
    }
    return json + "]";
}

JNIEXPORT jstring JNICALL
Java_com_kevinbatdorf_plugins_retroemulator_AresCore_nativeGetPortsJson(JNIEnv* env, jobject)
{
    // Available from staging on, no booted core required.
    if (!g_state || !g_state->system) return env->NewStringUTF("[]");
    return env->NewStringUTF(buildPortsJson().c_str());
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
    for (auto& screen : NodeUtil::findAll<ares::Node::Video::Screen>(g_state->root)) {
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
    return SaveIO::flush(g_state->cartridgePak, g_state->savePrefix) ? JNI_TRUE : JNI_FALSE;
}

// Phase 11 — supported systems ------------------------------------------------

/** Comma-separated ids of the systems compiled into this build (e.g. "fc,gb,gba,gbc,md,n64,sfc"). */
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
