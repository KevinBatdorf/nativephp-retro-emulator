#include "ares_ios_api.h"

#include <ares/ares.hpp>

#include "system_registry.hpp"
#include "node_util.hpp"
#include "save_io.hpp"
#include "cheat_parse.hpp"
#include "rate_control.hpp"
#include "core_options.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Emulator state — mirrors the Android EmulatorState.
// Video frames are stored in a heap buffer instead of a GL texture.
// ---------------------------------------------------------------------------
struct AresContext {
    const SystemRegistry::SystemDef* system = nullptr;

    std::shared_ptr<vfs::directory> systemPak;
    std::shared_ptr<vfs::directory> cartridgePak;

    // Slotted media (SuFami Turbo slots A/B, BS-X BS Memory). Slot ROM bytes are
    // staged by the bridge before loadRom; the pak is built + connected during
    // the boot and handed to the slot cartridge nodes by Platform::pak (parent
    // port name disambiguates A from B). Emulation thread only, like the paks.
    std::vector<uint8_t>            stagedSlot[2];
    std::shared_ptr<vfs::directory> slotPak[2];
    bool                            slotConnected[2] = {false, false};  // test seam

    ares::Node::System root;
    bool systemLoaded = false;
    bool romLoaded    = false;

    // Video — ARGB8888 pixels, written by the Platform::video() callback.
    std::vector<uint32_t> frameBuffer;
    uint32_t frameWidth  = 0;
    uint32_t frameHeight = 0;

    // Screen-node geometry captured alongside each frame. The renderer
    // computes presentation size from these exactly like
    // desktop-ui/program/platform.cpp:95-115.
    double nodeWidth  = 0.0;
    double nodeHeight = 0.0;
    double scaleX     = 1.0;
    double scaleY     = 1.0;
    double aspectX    = 1.0;
    double aspectY    = 1.0;
    uint32_t rotation = 0;

    // Overscan borders trimmed by default; setVideo(overscan: true) shows
    // them, mirroring desktop's Emulator::setOverscan.
    bool overscan = false;

    // Audio — mixed stereo floats, written by Platform::audio().
    // ~125 ms cap; overflow drops the OLDEST samples so backlog (audio lag)
    // self-heals instead of persisting (see the Android counterpart).
    std::mutex    audioMutex;
    std::vector<float> audioRingBuffer;
    std::vector<ares::Node::Audio::Stream> audioStreams;
    static constexpr size_t kAudioCap = 12000u;

    // Input — two per-port button masks OR'd together at poll time:
    //   hwMask — hardware controller bits, written by EmulatorInput.
    //   swMask — software bits, written natively by ares_press_button with the
    //            connected device's own bit for the named button.
    // Ports are 1-based; index [0]=port1 … up to kMaxPorts (multitap territory).
    static constexpr int kMaxPorts = 5;
    std::atomic<uint32_t> hwMask[kMaxPorts] {};
    std::atomic<uint32_t> swMask[kMaxPorts] {};

    // Cached mapping from an ares button node → (1-based port, bit). Built on
    // the emulation thread when a device is (re)connected. `bit` is the slot the
    // node reads; `defaultBit` is its unremapped value, kept so a remap
    // recomputes against defaults. Mutated + read on the emulation thread only.
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
    // centre-screen, same clamp as super-scope.cpp:52-56) so aimAt() can feed the
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
    // consumed on the emulation thread when inputRemapDirty is set (top of
    // ares_tick) or right after a device (re)connect.
    std::unordered_map<int, std::unordered_map<std::string, uint32_t>> inputRemap;
    std::mutex        inputRemapMutex;
    std::atomic<bool> inputRemapDirty {false};

    std::atomic<bool> paused {false};

    // Master audio volume (0–1) and balance (−1 left … +1 right), applied
    // when mixing into the ring buffer.
    std::atomic<float> volume  {1.0f};
    std::atomic<float> balance {0.0f};

    // ROM metadata (set once during ares_load_rom, read-only afterward).
    std::string romRegion;

    // Battery-save location ("<prefix>.save.ram", …); empty disables persistence.
    std::string savePrefix;

    // Cheats — every ctx access is serialized by the Swift-side emuLock, so
    // the emulation loop never reads these while a bridge call mutates them.
    // `cheats` keeps per-code pairs so ares_remove_cheat(code) works;
    // `cheatLookup` is the merged map the per-read hot path consults.
    std::map<std::string, std::map<uint32_t, uint32_t>> cheats;
    std::unordered_map<uint32_t, uint32_t> cheatLookup;

    auto rebuildCheatLookup() -> void {
        cheatLookup.clear();
        for (auto& [code, pairs] : cheats) {
            for (auto& [addr, value] : pairs) cheatLookup[addr] = value;
        }
    }

    // Rewind — desktop-ui/program/rewind.cpp semantics, run inside ares_tick.
    // Serialized by the Swift-side emuLock like every other ctx access.
    struct Rewind {
        bool enabled   = false;
        bool rewinding = false;
        uint32_t counter   = 0;
        uint32_t frequency = 10;   // capture every N frames (ares desktop default)
        uint32_t length    = 100;  // history cap (ares desktop default ≈ 16.7 s)
        std::vector<nall::serializer> history;
    } rewind;

    // Run-ahead — ares supports one hidden frame (desktop-ui program.cpp loop).
    // Suppressed while fast-forwarding or rewinding, like desktop.
    bool runAheadEnabled = false;
    std::atomic<bool> fastForwardActive {false};

    // Rumble — cores publish motor state via Platform::input() on rumble
    // nodes (SFC Rumble Gamepad, GB MBC5 carts, N64 Rumble Pak). Packed
    // strong<<16|weak; the host polls per frame and drives its haptics.
    std::atomic<bool> rumbleEnabled {false};
    std::atomic<uint32_t> rumbleState {0};

    // Dynamic rate control (see native/rate_control.hpp). Written from the
    // bridge thread, read in ares_tick on the emulation thread.
    std::atomic<bool> dynamicRateControl {true};

    // True refresh rate reported by the core via Platform::refreshRateHint
    // (region- and mode-aware: SFC NTSC 60.0988, GB 59.7275, PAL ~50, …).
    // 0 until the first hint; the Swift pacing loop polls it per iteration —
    // the analogue of ruby's Metal driver deriving its present interval
    // (ruby/video/metal/metal.cpp:118-151). Written on the emu thread.
    std::atomic<double> refreshRateHint {0.0};
};

static AresContext* g_ctx      = nullptr;

// ---------------------------------------------------------------------------
// IosPlatform — wires ares callbacks to the context.
// ---------------------------------------------------------------------------
struct IosPlatform : ares::Platform {

    auto attach(ares::Node::Object node) -> void override {
        if (!g_ctx) return;
        if (auto stream = NodeUtil::as<ares::Node::Audio::Stream>(node)) {
            stream->setResamplerFrequency(48000.0);
            g_ctx->audioStreams = NodeUtil::findAll<ares::Node::Audio::Stream>(g_ctx->root);
        }
    }

    auto detach(ares::Node::Object node) -> void override {
        if (!g_ctx) return;
        if (auto stream = NodeUtil::as<ares::Node::Audio::Stream>(node)) {
            g_ctx->audioStreams = NodeUtil::findAll<ares::Node::Audio::Stream>(g_ctx->root);
            std::erase(g_ctx->audioStreams, stream);
        }
    }

    auto pak(ares::Node::Object node) -> std::shared_ptr<vfs::directory> override {
        if (!g_ctx || !g_ctx->system) return {};
        // The system node IS the root — match by pointer, since some cores
        // rename it per model (PC Engine boots as "TurboGrafx 16" on NTSC-U).
        if (node == g_ctx->root) return g_ctx->systemPak;
        auto name = node->name();
        if (name == g_ctx->system->cartridgeNode.c_str()) return g_ctx->cartridgePak;
        // Sufami Turbo slot carts — same node name in both slots, so the parent
        // port ("Sufami Turbo Slot A" / "…B") picks which staged pak answers.
        if (name == "Sufami Turbo Cartridge") {
            auto parent = ares::Node::parent(node);
            std::string port = parent ? std::string((const char*)parent->name()) : "";
            if (port == "Sufami Turbo Slot A") return g_ctx->slotPak[0];
            if (port == "Sufami Turbo Slot B") return g_ctx->slotPak[1];
        }
        if (name == "BS Memory Cartridge") return g_ctx->slotPak[0];
        // Model-renamed cartridges ("TurboGrafx 16 Cartridge") — every core
        // names its main cartridge "<system> Cartridge", and the slot names
        // above are matched first.
        if (nall::string{name}.endsWith(" Cartridge")) return g_ctx->cartridgePak;
        return {};
    }

    auto video(ares::Node::Video::Screen node,
               const u32* data, u32 pitch, u32 width, u32 height) -> void override {
        if (!g_ctx) return;
        // NOTE: pitch is in BYTES (Screen::refresh passes width * sizeof(u32)).
        const u32 stride = pitch / sizeof(u32);
        g_ctx->frameWidth  = width;
        g_ctx->frameHeight = height;
        g_ctx->nodeWidth   = (double)node->width();
        g_ctx->nodeHeight  = (double)node->height();
        g_ctx->scaleX      = node->scaleX();
        g_ctx->scaleY      = node->scaleY();
        g_ctx->aspectX     = node->aspectX();
        g_ctx->aspectY     = node->aspectY();
        g_ctx->rotation    = node->rotation();
        g_ctx->frameBuffer.resize((size_t)width * height);
        for (u32 y = 0; y < height; y++) {
            std::memcpy(&g_ctx->frameBuffer[y * width],
                        data + y * stride, width * sizeof(u32));
        }
    }

    auto refreshRateHint(double refreshRate) -> void override {
        if (!g_ctx) return;
        // Log on change like ruby's Metal driver (metal.cpp:149-151).
        if (g_ctx->refreshRateHint.load(std::memory_order_relaxed) != refreshRate) {
            fprintf(stderr, "refresh rate hint changed to %f\n", refreshRate);
        }
        g_ctx->refreshRateHint.store(refreshRate, std::memory_order_relaxed);
    }

    auto audio(ares::Node::Audio::Stream) -> void override {
        if (!g_ctx || g_ctx->audioStreams.empty()) return;
        while (true) {
            for (auto& stream : g_ctx->audioStreams) {
                if (!stream->pending()) return;
            }
            f64 samples[2] = {0.0, 0.0};
            for (auto& stream : g_ctx->audioStreams) {
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
            const float volume  = g_ctx->volume.load(std::memory_order_relaxed);
            const float balance = g_ctx->balance.load(std::memory_order_relaxed);
            l *= volume * (balance > 0.0f ? 1.0f - balance : 1.0f);
            r *= volume * (balance < 0.0f ? 1.0f + balance : 1.0f);
            std::lock_guard<std::mutex> lock(g_ctx->audioMutex);
            g_ctx->audioRingBuffer.push_back(l);
            g_ctx->audioRingBuffer.push_back(r);
            if (g_ctx->audioRingBuffer.size() > AresContext::kAudioCap) {
                g_ctx->audioRingBuffer.erase(
                    g_ctx->audioRingBuffer.begin(),
                    g_ctx->audioRingBuffer.begin() +
                        (g_ctx->audioRingBuffer.size() - AresContext::kAudioCap));
            }
        }
    }

    auto input(ares::Node::Input::Input node) -> void override {
        if (!g_ctx) return;
        if (auto btn = NodeUtil::as<ares::Node::Input::Button>(node)) {
            for (auto& cached : g_ctx->inputCache) {
                if (cached.node == btn) {
                    int i = cached.port - 1;
                    uint32_t mask = g_ctx->hwMask[i].load(std::memory_order_relaxed)
                                  | g_ctx->swMask[i].load(std::memory_order_relaxed);
                    btn->setValue(mask & cached.bit);
                    return;
                }
            }
            return;
        }
        if (auto axis = NodeUtil::as<ares::Node::Input::Axis>(node)) {
            for (auto& cached : g_ctx->axisCache) {
                if (cached.node == axis) {
                    std::lock_guard<std::mutex> lock(g_ctx->axisMutex);
                    auto& acc = g_ctx->axisAccum[cached.port - 1][cached.name];
                    axis->setValue(acc);
                    acc = 0;   // consume: a relative delta applies once per poll
                    return;
                }
            }
            axis->setValue(0);
            return;
        }
        if (auto rumble = NodeUtil::as<ares::Node::Input::Rumble>(node)) {
            const uint32_t state = g_ctx->rumbleEnabled.load(std::memory_order_relaxed)
                ? (uint32_t)rumble->strongValue() << 16 | rumble->weakValue()
                : 0u;
            g_ctx->rumbleState.store(state, std::memory_order_relaxed);
        }
    }

    // Consulted by the cores on every CPU bus read — the empty() check keeps
    // the no-cheat hot path to a single branch.
    auto cheat(u32 address) -> maybe<u32> override {
        if (!g_ctx || g_ctx->cheatLookup.empty()) return nothing;
        auto it = g_ctx->cheatLookup.find(address);
        if (it != g_ctx->cheatLookup.end()) return it->second;
        return nothing;
    }
};

static IosPlatform* g_platform = nullptr;

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
// registry (and its button drift test) stays gamepad-only. Mirrors the Android
// table in ares_jni.cpp.
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
        {"pce", {
            // 6-button pad (avenuepad.cpp); shared names keep the gamepad's bits.
            {"Avenue Pad 6", {{{"II", 1u << 0}, {"Select", 1u << 2}, {"Run", 1u << 3},
                               {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6},
                               {"Right", 1u << 7}, {"I", 1u << 8}, {"III", 1u << 9},
                               {"IV", 1u << 10}, {"V", 1u << 11}, {"VI", 1u << 12}}, {}}},
            {"Multitap", {{}, {}}},
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
        {"pce", {"Multitap", 5}},
    };
    auto it = t.find(systemId);
    return it == t.end() ? nullptr : &it->second;
}
static bool isMultitap(const std::string& name) {
    if (!g_ctx || !g_ctx->system) return false;
    auto* m = multitapInfo(g_ctx->system->id);
    return m && name == m->name;
}
// Logical ports a physical port consumes: the multitap's width, else 1.
static int portBlock(const std::string& name) {
    if (!isMultitap(name)) return 1;
    return multitapInfo(g_ctx->system->id)->block;
}

// Cache a device's button + axis nodes under `parent` for `port` (1-based).
static void cacheDevice(ares::Node::Object parent,
                        const DeviceDescriptor& desc, int port) {
    for (auto& btn : NodeUtil::findAll<ares::Node::Input::Button>(parent)) {
        auto it = desc.buttons.find(std::string((const char*)btn->name()));
        if (it != desc.buttons.end())
            g_ctx->inputCache.push_back({btn, port, it->second, it->second});
    }
    for (auto& ax : NodeUtil::findAll<ares::Node::Input::Axis>(parent)) {
        auto name = std::string((const char*)ax->name());
        if (std::find(desc.axes.begin(), desc.axes.end(), name) != desc.axes.end())
            g_ctx->axisCache.push_back({ax, port, name});
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
// stored per-port remap. Emulation thread only (cached.bit is not atomic).
static void applyInputRemap() {
    if (!g_ctx) return;
    std::lock_guard<std::mutex> lock(g_ctx->inputRemapMutex);
    for (auto& cached : g_ctx->inputCache) {
        cached.bit = cached.defaultBit;
        auto pit = g_ctx->inputRemap.find(cached.port);
        if (pit == g_ctx->inputRemap.end()) continue;
        auto it = pit->second.find(toLower(std::string((const char*)cached.node->name())));
        if (it != pit->second.end()) cached.bit = it->second;
    }
}

// Rebuild the input/axis caches to match connectedDevice[] on the live core.
// Emulation thread only. Systems with built-in controls (ports == 0, e.g.
// Game Boy) always cache their controls on the system node. Otherwise every
// registered device is (re)allocated on its hot-swappable port; empty ports
// get nothing.
static void applyConnectedDevices() {
    if (!g_ctx || !g_ctx->system || !g_ctx->root) return;
    auto& def = *g_ctx->system;
    g_ctx->inputCache.clear();
    g_ctx->axisCache.clear();

    if (def.ports == 0) {   // built-in controls (Game Boy) — always on logical 1
        DeviceDescriptor desc; desc.buttons = def.buttons;
        cacheDevice(g_ctx->root, desc, 1);
        applyInputRemap();
        return;
    }

    // Walk physical ports, expanding a multitap into 4 gamepad sub-ports, and
    // assign consecutive LOGICAL port numbers (a multitap on port 2 → 2,3,4,5).
    int logical = 1;
    for (int p = 1; p <= def.ports && logical <= AresContext::kMaxPorts; p++) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(g_ctx->deviceMutex);
            name = g_ctx->connectedDevice[p - 1];
        }
        auto portName = std::string("Controller Port ") + std::to_string(p);
        auto port = NodeUtil::findByName<ares::Node::Port>(g_ctx->root, portName.c_str());
        // Single-port cores name theirs plain "Controller Port" (PC Engine).
        if (!port && def.ports == 1) {
            port = NodeUtil::findByName<ares::Node::Port>(g_ctx->root, "Controller Port");
        }

        if (name.empty()) { if (port) port->disconnect(); logical += 1; continue; }
        if (!port) { logical += portBlock(name); continue; }

        if (isMultitap(name)) {
            port->allocate(name.c_str());
            port->connect();
            auto tap = NodeUtil::connected(port);   // the multitap peripheral
            int block = portBlock(name);
            DeviceDescriptor gp; gp.buttons = def.buttons;   // each sub-port is a gamepad
            for (int i = 1; i <= block && logical <= AresContext::kMaxPorts; i++, logical++) {
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

// Resolve the descriptor of the device at a LOGICAL port (a multitap sub-port
// reads "Gamepad"), or the built-in controls for a ports==0 system. Computed
// on-demand from the synchronously-set connectedDevice[] so it never races the
// deferred allocate. Returns false if nothing is connected there.
static bool connectedDescriptor(int port, DeviceDescriptor& out) {
    if (!g_ctx || !g_ctx->system) return false;
    auto& def = *g_ctx->system;
    if (def.ports == 0) { out.buttons = def.buttons; out.axes.clear(); return true; }
    std::string name;
    {
        std::lock_guard<std::mutex> lock(g_ctx->deviceMutex);
        int logical = 1;
        for (int p = 1; p <= def.ports && logical <= AresContext::kMaxPorts; p++) {
            auto& dev = g_ctx->connectedDevice[p - 1];
            int block = portBlock(dev);
            for (int i = 0; i < block && logical <= AresContext::kMaxPorts; i++, logical++) {
                if (logical == port) { name = isMultitap(dev) ? std::string("Gamepad") : dev; }
            }
            if (!name.empty()) break;
        }
    }
    if (name.empty()) return false;
    return resolveDevice(def, name, out);
}

// ---------------------------------------------------------------------------
// C API implementation
// ---------------------------------------------------------------------------
extern "C" {

extern "C" int retro_emulator_static_cores();

AresContext* ares_create(void) {
    // Forces the statically-linked core objects (and their Registrars) into
    // the image — see core_link.cpp.
    (void)retro_emulator_static_cores();
    if (!g_platform) {
        g_ctx      = new AresContext{};
        g_platform = new IosPlatform{};
        ares::platform = g_platform;
    }
    return g_ctx;
}

void ares_destroy(AresContext* ctx) {
    if (!ctx || ctx != g_ctx) return;
    if (g_ctx->systemLoaded && g_ctx->root) {
        // unload() joins worker threads (e.g. the video screen thread) before
        // the node tree is torn down — dropping root without it lets those
        // threads race into freed state and crash.
        g_ctx->root->unload();
        g_ctx->root.reset();
    }
    // Works around an upstream ares bug — a system loaded but never powered
    // strands dangling Thread entry points that wedge or corrupt a later
    // load. Full story on the declaration (system_registry.hpp).
    SystemRegistry::clearStaleEntryPoints();
    ares::platform = nullptr;
    delete g_platform; g_platform = nullptr;
    delete g_ctx;      g_ctx      = nullptr;
}

void ares_reset(AresContext* ctx) {
    if (!ctx || ctx != g_ctx) return;
    if (ctx->root) {
        ctx->root->unload();
        ctx->root.reset();
    }
    SystemRegistry::clearStaleEntryPoints();
    // Factory state in place — Android's Stop cycles destroy()+init(), which
    // resets every preference atomic; destruct + placement-new matches that
    // while keeping the pointer the host (audio/input) stored valid.
    ctx->~AresContext();
    new (ctx) AresContext{};
}

const char* ares_supported_systems(void) {
    static std::string ids = [] {
        std::string s;
        for (auto* def : SystemRegistry::all()) {
            if (!s.empty()) s += ",";
            s += def->id;
        }
        return s;
    }();
    return ids.c_str();
}

// 4b — system staging + ROM-first boot. No core boots without a ROM:
// ares_load_system only STAGES the declaration, ares_load_rom is the one boot
// path — first load and every swap alike — mirroring desktop, which builds a
// fresh system per game load with the region already known from the ROM
// analysis (desktop-ui/emulator/emulator.cpp:40-60, super-famicom.cpp:125-126).

bool ares_load_system(AresContext* ctx, const char* system_id) {
    if (!ctx || !system_id) return false;

    auto* def = SystemRegistry::find(system_id);
    if (!def) {
        fprintf(stderr, "ares_load_system: unsupported system '%s'\n", system_id);
        return false;
    }

    // Stage only — re-staging over a running core is legal; the running game
    // continues until the next ares_load_rom boots the new declaration.
    ctx->system = def;
    return true;
}

/**
 * Unload a running core in place, keeping the context (frame buffers, AV/pref
 * atomics) alive so the next boot inherits them. Follows the ares_destroy
 * teardown order: flush saves, join worker threads via root->unload(), then
 * clear the per-boot state that must not leak into a fresh core.
 */
static void unloadCore(AresContext* ctx)
{
    if (ctx->romLoaded && ctx->cartridgePak) {
        SaveIO::flush(ctx->cartridgePak, ctx->savePrefix);
    }
    if (ctx->root) {
        ctx->root->unload();
        ctx->root.reset();
    }
    SystemRegistry::clearStaleEntryPoints();

    ctx->systemLoaded = false;
    ctx->romLoaded    = false;
    ctx->systemPak.reset();
    ctx->cartridgePak.reset();
    ctx->slotPak[0].reset();
    ctx->slotPak[1].reset();
    ctx->slotConnected[0] = ctx->slotConnected[1] = false;
    ctx->inputCache.clear();
    ctx->axisCache.clear();
    // connectedDevice[] intentionally survives (registrations persist across a
    // reboot); the swMask does not — a device teardown drops held buttons.
    for (int i = 0; i < AresContext::kMaxPorts; i++) {
        ctx->swMask[i].store(0, std::memory_order_relaxed);
    }
    ctx->audioStreams.clear();
    {
        std::lock_guard<std::mutex> lock(ctx->audioMutex);
        ctx->audioRingBuffer.clear();
    }
    // Game knowledge dies with the core (plan 4b): cheats, rewind timeline,
    // the stale refresh hint, and any paused flag from the old game.
    ctx->cheats.clear();
    ctx->cheatLookup.clear();
    ctx->rewind.rewinding = false;
    ctx->rewind.counter   = 0;
    ctx->rewind.history.clear();
    ctx->refreshRateHint.store(0.0, std::memory_order_relaxed);
    ctx->paused.store(false, std::memory_order_relaxed);
}

int ares_load_rom(AresContext* ctx, const uint8_t* rom, size_t rom_size,
                  const char* save_prefix,
                  const char* region_override, const char* preferred_regions) {
    if (!ctx || !ctx->system || !rom || rom_size == 0) return 0;
    auto* def = ctx->system;

    // Analyze BEFORE any teardown — a ROM that fails analysis must leave a
    // running game untouched (failures after teardown starts end in the clean
    // stopped state instead).
    auto built = def->makeCartridgePak(rom, rom_size);
    if (!built) {
        fprintf(stderr, "ares_load_rom: %s\n", built.error.c_str());
        return 0;
    }

    auto region = SystemRegistry::resolveRegion(
        *def, built.region,
        region_override ? region_override : "",
        preferred_regions ? preferred_regions : "");
    auto loadName = SystemRegistry::loadNameFor(*def, region);

    // Fresh core per game, like desktop.
    if (ctx->systemLoaded) unloadCore(ctx);

    ctx->systemPak = def->makeSystemPak(*def);
    if (!def->load(ctx->root, *def, loadName)) {
        fprintf(stderr, "ares_load_rom: ares load failed: %s\n", loadName.c_str());
        unloadCore(ctx);
        return -1;
    }

    // Controllers are registered explicitly (ares_connect_device), not
    // auto-allocated: re-connect whatever the dev registered before this boot
    // (persists across loadRom) and build its caches. A system with no
    // registrations boots with no input; built-in-controls systems (ports == 0)
    // always cache their node.
    applyConnectedDevices();
    ctx->inputRemapDirty.store(false, std::memory_order_relaxed);
    ctx->deviceDirty.store(false, std::memory_order_relaxed);

    // Desktop applies its overscan setting to every screen on load
    // (emulator.cpp:137 → setOverscan scan, emulator.cpp:242-246). Cores
    // consult screen->overscan() per frame, so this takes effect immediately.
    for (auto& screen : NodeUtil::findAll<ares::Node::Video::Screen>(ctx->root)) {
        screen->setOverscan(ctx->overscan);
    }
    ctx->systemLoaded = true;

    ctx->cartridgePak = built.pak;

    // Seed battery saves from disk before the boards read the pak at connect.
    ctx->savePrefix = save_prefix ? save_prefix : "";
    SaveIO::seed(ctx->cartridgePak, ctx->savePrefix);

    auto cartridgeSlot = NodeUtil::findByName<ares::Node::Port>(ctx->root, "Cartridge Slot");
    if (!cartridgeSlot) {
        unloadCore(ctx);
        return -1;
    }
    auto baseCartridge = cartridgeSlot->allocate();
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
        if (ctx->stagedSlot[i].empty()) continue;
        if (!def->makeSlotPak) { fprintf(stderr, "system '%s' has no slot pak builder\n", def->id.c_str()); continue; }
        auto slot = baseCartridge ? NodeUtil::findByName<ares::Node::Port>(baseCartridge, slots[i].port)
                                   : ares::Node::Port();
        if (!slot) { fprintf(stderr, "slot port '%s' not found\n", slots[i].port); continue; }
        ctx->slotPak[i] = def->makeSlotPak(
            i, slots[i].flash, ctx->stagedSlot[i].data(), ctx->stagedSlot[i].size());
        slot->allocate();
        slot->connect();
        ctx->slotConnected[i] = (bool)NodeUtil::connected(slot);
        ctx->stagedSlot[i].clear();   // pak copied the bytes
    }

    ctx->root->power(false);
    ctx->romLoaded = true;
    // Report the BOOTED region — for region-free systems (gb) fall back to
    // whatever the analyzer said (usually empty).
    ctx->romRegion = region.empty() ? built.region : region;
    return 1;
}

const char* ares_system_extensions(AresContext*, const char* system_id) {
    static thread_local std::string exts;
    exts.clear();
    if (auto* def = SystemRegistry::find(system_id ? system_id : "")) {
        for (auto& ext : def->extensions) {
            if (!exts.empty()) exts += ",";
            exts += ext;
        }
    }
    return exts.c_str();
}

bool ares_add_cheat(AresContext* ctx, const char* code) {
    if (!ctx || !code) return false;
    auto pairs = CheatParse::parse(code);
    if (pairs.empty()) return false;
    ctx->cheats[code] = std::move(pairs);
    ctx->rebuildCheatLookup();
    return true;
}

bool ares_remove_cheat(AresContext* ctx, const char* code) {
    if (!ctx || !code) return false;
    bool removed = ctx->cheats.erase(code) > 0;
    if (removed) ctx->rebuildCheatLookup();
    return removed;
}

void ares_clear_cheats(AresContext* ctx) {
    if (!ctx) return;
    ctx->cheats.clear();
    ctx->cheatLookup.clear();
}

void ares_set_audio(AresContext* ctx, float volume, float balance) {
    if (!ctx) return;
    ctx->volume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
    ctx->balance.store(std::clamp(balance, -1.0f, 1.0f), std::memory_order_relaxed);
}

void ares_set_video(AresContext* ctx, float luminance, float saturation,
                    float gamma, bool color_bleed, bool overscan) {
    if (!ctx || !ctx->systemLoaded) return;
    ctx->overscan = overscan;
    for (auto& screen : NodeUtil::findAll<ares::Node::Video::Screen>(ctx->root)) {
        screen->setLuminance((f64)luminance);
        screen->setSaturation((f64)saturation);
        screen->setGamma((f64)gamma);
        screen->setColorBleed(color_bleed);
        screen->setOverscan(overscan);
    }
}

// Apply a per-system emulation toggle; no-ops when the core lacks the node.
void ares_set_core_boolean(AresContext* ctx, const char* key, bool value) {
    if (!ctx || !ctx->systemLoaded || !key) return;
    CoreOptions::applyBoolean(ctx->root, key, value);
}

// Test seam: a toggle's current node value (1/0), or -1 if absent.
int ares_get_core_boolean(AresContext* ctx, const char* key) {
    if (!ctx || !ctx->systemLoaded || !key) return -1;
    return CoreOptions::readBoolean(ctx->root, key);
}

double ares_get_refresh_rate_hint(AresContext* ctx) {
    return ctx ? ctx->refreshRateHint.load(std::memory_order_relaxed) : 0.0;
}

void ares_get_video_geometry(AresContext* ctx, double out[7]) {
    out[0] = 0; out[1] = 0; out[2] = 1; out[3] = 1;
    out[4] = 1; out[5] = 1; out[6] = 0;
    if (!ctx) return;
    out[0] = ctx->nodeWidth;
    out[1] = ctx->nodeHeight;
    out[2] = ctx->scaleX;
    out[3] = ctx->scaleY;
    out[4] = ctx->aspectX;
    out[5] = ctx->aspectY;
    out[6] = (double)ctx->rotation;
}

bool ares_flush_saves(AresContext* ctx) {
    if (!ctx || !ctx->romLoaded || ctx->savePrefix.empty()) return false;
    // System::save() writes board memory back into the cartridge pak.
    ctx->root->save();
    return SaveIO::flush(ctx->cartridgePak, ctx->savePrefix);
}

// Port of desktop-ui rewindRun(): in normal play, snapshot every `frequency`
// frames into a bounded ring; while rewinding, pop and restore at 5× the
// capture rate until the history is exhausted, then fall back to play.
static void rewindRun(AresContext* ctx) {
    auto& rw = ctx->rewind;
    if (!rw.enabled) return;

    if (!rw.rewinding) {
        if (++rw.counter < rw.frequency) return;
        rw.counter = 0;
        if (rw.history.size() >= rw.length) rw.history.erase(rw.history.begin());
        rw.history.push_back(ctx->root->serialize(false));
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
    ctx->root->unserialize(s);
    if (rw.history.empty()) rw.rewinding = false;
}

bool ares_tick(AresContext* ctx) {
    if (!ctx || !ctx->romLoaded) return false;

    // Consume pending controller changes on the emulation thread (the bridge
    // thread only stored them + flagged). A device (re)connect rebuilds the
    // caches (which re-applies the remap); a lone remap just recomputes bits.
    // Runs even while paused; the read side (Platform::input) is this thread.
    if (ctx->deviceDirty.exchange(false, std::memory_order_relaxed)) {
        applyConnectedDevices();
        ctx->inputRemapDirty.store(false, std::memory_order_relaxed);
    } else if (ctx->inputRemapDirty.exchange(false, std::memory_order_relaxed)) {
        applyInputRemap();
    }

    if (ctx->paused.load(std::memory_order_relaxed)) return false;

    // DRC gates off during fast-forward, porting desktop's FastForwardOn ->
    // ruby::audio.setDynamic(false) (platform.cpp:29-45).
    if (ctx->dynamicRateControl.load(std::memory_order_relaxed) &&
        !ctx->fastForwardActive.load(std::memory_order_relaxed) &&
        !ctx->audioStreams.empty()) {
        f64 fill;
        {
            std::lock_guard<std::mutex> lock(ctx->audioMutex);
            fill = (f64)ctx->audioRingBuffer.size() / AresContext::kAudioCap;
        }
        RateControl::apply(ctx->audioStreams, fill);
    }

    rewindRun(ctx);

    // Desktop-ui run-ahead loop: run one hidden frame (video/audio suppressed
    // by the cores), snapshot, run the visible frame, roll back — a one-frame
    // preview that reduces perceived input latency at 2× emulation cost.
    const bool runAhead = ctx->runAheadEnabled &&
        !ctx->rewind.rewinding &&
        !ctx->fastForwardActive.load(std::memory_order_relaxed);
    if (!runAhead) {
        ctx->root->run();
    } else {
        ares::setRunAhead(true);
        ctx->root->run();
        auto state = ctx->root->serialize(false);
        ares::setRunAhead(false);
        ctx->root->run();
        state.setReading();
        ctx->root->unserialize(state);
    }
    return true;
}

void ares_configure_rewind(AresContext* ctx, bool enabled, int buffer_seconds) {
    if (!ctx) return;
    auto& rw = ctx->rewind;
    rw.enabled = enabled;
    rw.length  = buffer_seconds > 0 ? (uint32_t)buffer_seconds * 6u : 100u;
    if (!enabled) {
        rw.rewinding = false;
        rw.counter = 0;
        rw.history.clear();
    }
}

int ares_toggle_rewind(AresContext* ctx) {
    if (!ctx || !ctx->rewind.enabled) return -1;
    auto& rw = ctx->rewind;
    rw.rewinding = !rw.rewinding;
    rw.counter = 0;
    return rw.rewinding ? 1 : 0;
}

void ares_set_run_ahead(AresContext* ctx, bool enabled) {
    if (ctx) ctx->runAheadEnabled = enabled;
}

void ares_set_fast_forward(AresContext* ctx, bool active) {
    if (ctx) ctx->fastForwardActive.store(active, std::memory_order_relaxed);
}

void ares_set_dynamic_rate_control(AresContext* ctx, bool enabled) {
    if (ctx) ctx->dynamicRateControl.store(enabled, std::memory_order_relaxed);
}

void ares_set_rumble_enabled(AresContext* ctx, bool enabled) {
    if (!ctx) return;
    ctx->rumbleEnabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) ctx->rumbleState.store(0, std::memory_order_relaxed);
}

uint32_t ares_get_rumble_state(AresContext* ctx) {
    return ctx ? ctx->rumbleState.load(std::memory_order_relaxed) : 0;
}

void ares_set_input(AresContext* ctx, int port, uint32_t bits) {
    if (!ctx || port < 1 || port > AresContext::kMaxPorts) return;
    ctx->hwMask[port - 1].store(bits, std::memory_order_relaxed);
}

uint32_t ares_get_input(AresContext* ctx, int port) {
    if (!ctx || port < 1 || port > AresContext::kMaxPorts) return 0;
    int i = port - 1;
    return ctx->hwMask[i].load(std::memory_order_relaxed)
         | ctx->swMask[i].load(std::memory_order_relaxed);
}

void ares_stage_slot(AresContext* ctx, int index, const uint8_t* rom, size_t rom_size) {
    if (!ctx || index < 0 || index > 1) return;
    if (!rom || rom_size == 0) {
        ctx->stagedSlot[index].clear();
        return;
    }
    ctx->stagedSlot[index].assign(rom, rom + rom_size);
}

bool ares_is_slot_connected(AresContext* ctx, int index) {
    if (!ctx || index < 0 || index > 1) return false;
    return ctx->slotConnected[index];
}

// Status-string returns share one thread-local buffer (copied by the caller
// before the next call on the same thread, per the header contract).
static const char* statusRet(const std::string& s) {
    static thread_local std::string buf;
    buf = s;
    return buf.c_str();
}

const char* ares_connect_device(AresContext* ctx, const char* system_id,
                                int port, const char* device) {
    if (!ctx) return statusRet("SYSTEM_NOT_LOADED");

    // Validate against the staged system by id (from the registry, static) so
    // we never race an in-flight staging — mirrors the Android JNI.
    auto* def = SystemRegistry::find(system_id ? system_id : "");
    if (!def) return statusRet("SYSTEM_NOT_LOADED");

    // Built-in-controls systems (Game Boy) have no ports to plug into — the
    // controls are always present, so a connect is a harmless no-op.
    if (def->ports == 0) return statusRet("");

    int maxPort = std::min(def->ports, AresContext::kMaxPorts);
    if (port < 1 || port > maxPort) return statusRet("INVALID_PARAMETERS");

    std::string name = device ? device : "";
    DeviceDescriptor desc;
    if (!name.empty() && !resolveDevice(*def, name, desc)) return statusRet("UNSUPPORTED_DEVICE");

    {
        std::lock_guard<std::mutex> lock(ctx->deviceMutex);
        ctx->connectedDevice[port - 1] = name;
    }
    {
        // Reset the light-gun shadow cursor to centre, matching ares' fresh
        // device (super-scope.hpp init). Telescoping deltas keep it in sync even
        // if aimAt runs before the deferred allocate.
        std::lock_guard<std::mutex> lock(ctx->axisMutex);
        ctx->lightgunX[port - 1] = AresContext::kGunW / 2;
        ctx->lightgunY[port - 1] = AresContext::kGunH / 2;
    }
    ctx->deviceDirty.store(true, std::memory_order_relaxed);
    return statusRet("");
}

int ares_device_ports(AresContext* ctx, const char* system_id,
                      int physical, int* out, int capacity) {
    if (!ctx || !out || capacity <= 0) return 0;
    auto* def = SystemRegistry::find(system_id ? system_id : "");
    int count = 0;
    if (def) {
        int logical = 1;
        for (int p = 1; p <= def->ports && logical <= AresContext::kMaxPorts; p++) {
            std::string name;
            {
                std::lock_guard<std::mutex> lock(ctx->deviceMutex);
                name = ctx->connectedDevice[p - 1];
            }
            int block = portBlock(name);
            for (int i = 0; i < block && logical <= AresContext::kMaxPorts; i++, logical++) {
                if (p == physical && count < capacity) out[count++] = logical;
            }
        }
    }
    return count;
}

const char* ares_press_button(AresContext* ctx, int port,
                              const char* name, bool down) {
    if (!ctx || !ctx->system) return statusRet("SYSTEM_NOT_LOADED");
    if (port < 1 || port > AresContext::kMaxPorts) return statusRet("INVALID_PARAMETERS");

    DeviceDescriptor desc;
    std::string btnName = name ? name : "";
    uint32_t bit;
    if (!connectedDescriptor(port, desc) || !bitForButtonName(desc, btnName, bit))
        return statusRet(std::string("UNKNOWN_BUTTON:") + btnName);

    if (down) ctx->swMask[port - 1].fetch_or(bit, std::memory_order_relaxed);
    else      ctx->swMask[port - 1].fetch_and(~bit, std::memory_order_relaxed);
    return statusRet("");
}

const char* ares_set_axis(AresContext* ctx, int port, const char* name, int value) {
    if (!ctx || !ctx->system) return statusRet("SYSTEM_NOT_LOADED");
    if (port < 1 || port > AresContext::kMaxPorts) return statusRet("INVALID_PARAMETERS");

    DeviceDescriptor desc;
    std::string axisName = name ? name : "";
    if (!connectedDescriptor(port, desc) ||
        std::find(desc.axes.begin(), desc.axes.end(), axisName) == desc.axes.end())
        return statusRet("INVALID_PARAMETERS");

    std::lock_guard<std::mutex> lock(ctx->axisMutex);
    ctx->axisAccum[port - 1][axisName] += value;
    return statusRet("");
}

const char* ares_aim_at(AresContext* ctx, int port, float nx, float ny) {
    if (!ctx || !ctx->system) return statusRet("SYSTEM_NOT_LOADED");
    if (port < 1 || port > AresContext::kMaxPorts) return statusRet("INVALID_PARAMETERS");

    DeviceDescriptor desc;
    auto has = [&](const char* a) {
        return std::find(desc.axes.begin(), desc.axes.end(), a) != desc.axes.end();
    };
    if (!connectedDescriptor(port, desc) || !has("X") || !has("Y"))
        return statusRet("INVALID_PARAMETERS");

    float cx = nx < 0 ? 0 : (nx > 1 ? 1 : nx);
    float cy = ny < 0 ? 0 : (ny > 1 ? 1 : ny);
    int tx = (int)(cx * AresContext::kGunW);
    int ty = (int)(cy * AresContext::kGunH);

    std::lock_guard<std::mutex> lock(ctx->axisMutex);
    ctx->axisAccum[port - 1]["X"] += tx - ctx->lightgunX[port - 1];
    ctx->axisAccum[port - 1]["Y"] += ty - ctx->lightgunY[port - 1];
    ctx->lightgunX[port - 1] = tx;   // target is in-bounds (0..W/0..H)
    ctx->lightgunY[port - 1] = ty;
    return statusRet("");
}

const char* ares_set_input_mapping(AresContext* ctx, int port,
                                   const char* const* emulated,
                                   const char* const* source, int count) {
    if (!ctx || !ctx->system) return statusRet("SYSTEM_NOT_LOADED");
    auto& def = *ctx->system;
    (void)def;

    if (port < 1 || port > AresContext::kMaxPorts) return statusRet("INVALID_PARAMETERS");
    if (count > 0 && (!emulated || !source)) return statusRet("INVALID_PARAMETERS");

    // Remap names belong to the device at this logical port (multitap-aware).
    DeviceDescriptor desc;
    if (!connectedDescriptor(port, desc))
        return statusRet("INVALID_PARAMETERS"); // no controller registered on this port

    // Resolve+validate the whole batch before mutating any state, so a bad entry
    // leaves the existing remap untouched.
    std::unordered_map<std::string, uint32_t> resolved;
    for (int i = 0; i < count; i++) {
        std::string emuName = emulated[i] ? emulated[i] : "";
        std::string srcName = source[i] ? source[i] : "";
        uint32_t emuBit, srcBit;
        if (!bitForButtonName(desc, emuName, emuBit))
            return statusRet(std::string("UNKNOWN_BUTTON:") + emuName);
        if (!bitForButtonName(desc, srcName, srcBit))
            return statusRet(std::string("UNKNOWN_BUTTON:") + srcName);
        resolved[toLower(emuName)] = srcBit;
    }

    {
        std::lock_guard<std::mutex> lock(ctx->inputRemapMutex);
        if (count == 0) {
            ctx->inputRemap.erase(port);
        } else {
            auto& portMap = ctx->inputRemap[port];
            for (auto& [name, bit] : resolved) portMap[name] = bit;
        }
    }
    ctx->inputRemapDirty.store(true, std::memory_order_relaxed);
    return statusRet("");
}

int ares_get_button_bit(AresContext* ctx, int port, const char* name) {
    if (!ctx || !name) return -1;
    auto lname = toLower(name);
    for (auto& cached : ctx->inputCache) {
        if (cached.port != port) continue;
        if (toLower(std::string((const char*)cached.node->name())) == lname)
            return (int)cached.bit;
    }
    return -1;
}

int ares_get_axis_accum(AresContext* ctx, int port, const char* name) {
    if (!ctx || !name || port < 1 || port > AresContext::kMaxPorts) return 0;
    std::lock_guard<std::mutex> lock(ctx->axisMutex);
    auto& m = ctx->axisAccum[port - 1];
    auto it = m.find(name);
    return it == m.end() ? 0 : (int)it->second;
}

bool ares_get_frame(AresContext* ctx,
                    uint32_t* out_buf, size_t buf_capacity,
                    uint32_t* out_width, uint32_t* out_height)
{
    if (!ctx || ctx->frameBuffer.empty()) {
        if (out_width)  *out_width  = 0;
        if (out_height) *out_height = 0;
        return false;
    }
    if (out_width)  *out_width  = ctx->frameWidth;
    if (out_height) *out_height = ctx->frameHeight;
    if (out_buf && buf_capacity > 0) {
        size_t count = std::min(buf_capacity,
                                (size_t)ctx->frameWidth * ctx->frameHeight);
        std::memcpy(out_buf, ctx->frameBuffer.data(), count * sizeof(uint32_t));
    }
    return true;
}

size_t ares_read_audio(AresContext* ctx, float* out, size_t capacity) {
    if (!ctx || !out || capacity == 0) return 0;
    std::lock_guard<std::mutex> lock(ctx->audioMutex);
    if (ctx->audioRingBuffer.empty()) return 0;
    size_t count = std::min(capacity, ctx->audioRingBuffer.size());
    std::memcpy(out, ctx->audioRingBuffer.data(), count * sizeof(float));
    ctx->audioRingBuffer.erase(
        ctx->audioRingBuffer.begin(),
        ctx->audioRingBuffer.begin() + (ptrdiff_t)count);
    return count;
}

void ares_pause(AresContext* ctx) {
    if (ctx) ctx->paused.store(true, std::memory_order_relaxed);
}

void ares_resume(AresContext* ctx) {
    if (ctx) ctx->paused.store(false, std::memory_order_relaxed);
}

bool ares_state_save(AresContext* ctx, const char* path) {
    if (!ctx || !ctx->romLoaded || !path) return false;
    try {
        auto s = ctx->root->serialize(false);
        FILE* f = std::fopen(path, "wb");
        if (!f) return false;
        std::fwrite(s.data(), 1, s.size(), f);
        std::fclose(f);
        return true;
    } catch (...) { return false; }
}

bool ares_state_load(AresContext* ctx, const char* path) {
    if (!ctx || !ctx->romLoaded || !path) return false;
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long fileSize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fileSize <= 0) { std::fclose(f); return false; }
    std::vector<u8> data((size_t)fileSize);
    std::fread(data.data(), 1, (size_t)fileSize, f);
    std::fclose(f);
    try {
        nall::serializer s{data.data(), (u32)fileSize};
        return ctx->root->unserialize(s);
    } catch (...) { return false; }
}

int ares_read_memory(AresContext* ctx, uint32_t address, uint8_t* out, int length) {
    if (!ctx || !ctx->romLoaded || !ctx->system || !out || length <= 0) return -1;

    auto* def = ctx->system;
    uint32_t len = (uint32_t)length;
    if (address < def->memBase) return -1;
    uint32_t offset = address - def->memBase;
    if (offset + len > def->memSize) return -1;

    for (uint32_t i = 0; i < len; i++) out[i] = def->memRead(offset + i);
    return (int)len;
}

void ares_write_memory(AresContext* ctx, uint32_t address,
                       const uint8_t* bytes, int length)
{
    if (!ctx || !ctx->romLoaded || !ctx->system || !bytes || length <= 0) return;

    auto* def = ctx->system;
    uint32_t len = (uint32_t)length;
    if (address < def->memBase) return;
    uint32_t offset = address - def->memBase;
    if (offset + len > def->memSize) return;

    for (uint32_t i = 0; i < len; i++) def->memWrite(offset + i, bytes[i]);
}

const char* ares_get_region(AresContext* ctx) {
    if (!ctx) return "";
    return ctx->romRegion.c_str();
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
static std::string buildPortsJson(AresContext* ctx) {
    auto& def = *ctx->system;
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
    for (int p = 1; p <= def.ports && logical <= AresContext::kMaxPorts; p++) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(ctx->deviceMutex);
            name = ctx->connectedDevice[p - 1];
        }
        if (isMultitap(name)) {   // fans out to gamepad logical ports
            DeviceDescriptor gp; gp.buttons = def.buttons;
            int block = portBlock(name);
            for (int i = 0; i < block && logical <= AresContext::kMaxPorts; i++, logical++)
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

const char* ares_get_ports_json(AresContext* ctx) {
    // Registry data + registrations — available from staging on, no booted
    // core required. Thread-local storage, same contract as the other strings.
    if (!ctx || !ctx->system) return "[]";
    static thread_local std::string json;
    json = buildPortsJson(ctx);
    return json.c_str();
}

} // extern "C"
