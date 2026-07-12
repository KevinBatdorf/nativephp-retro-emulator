#include "ares_ios_api.h"

#include <ares/ares.hpp>

#include "system_registry.hpp"
#include "save_io.hpp"
#include "cheat_parse.hpp"
#include "rate_control.hpp"

#include <algorithm>
#include <atomic>
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

    // Input — written from any thread, read from the emulation thread.
    std::atomic<uint32_t> inputMaskPort1 {0};
    std::atomic<uint32_t> inputMaskPort2 {0};

    struct CachedButton {
        ares::Node::Input::Button node;
        std::atomic<uint32_t>*   mask;
        uint32_t                 bit;
    };
    std::vector<CachedButton> inputCache;

    std::atomic<bool> paused {false};

    // Master audio volume (0–1) and balance (−1 left … +1 right), applied
    // when mixing into the ring buffer.
    std::atomic<float> volume  {1.0f};
    std::atomic<float> balance {0.0f};

    // ROM metadata (set once during ares_load_rom, read-only afterward).
    std::string romRegion;
    std::string portsJson;

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
        if (auto stream = node->cast<ares::Node::Audio::Stream>()) {
            stream->setResamplerFrequency(48000.0);
            g_ctx->audioStreams = g_ctx->root->find<ares::Node::Audio::Stream>();
        }
    }

    auto detach(ares::Node::Object node) -> void override {
        if (!g_ctx) return;
        if (auto stream = node->cast<ares::Node::Audio::Stream>()) {
            g_ctx->audioStreams = g_ctx->root->find<ares::Node::Audio::Stream>();
            std::erase(g_ctx->audioStreams, stream);
        }
    }

    auto pak(ares::Node::Object node) -> std::shared_ptr<vfs::directory> override {
        if (!g_ctx || !g_ctx->system) return {};
        auto name = node->name();
        if (name == g_ctx->system->systemNode.c_str())    return g_ctx->systemPak;
        if (name == g_ctx->system->cartridgeNode.c_str()) return g_ctx->cartridgePak;
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
        if (auto btn = node->cast<ares::Node::Input::Button>()) {
            for (auto& cached : g_ctx->inputCache) {
                if (cached.node == btn) {
                    btn->setValue(
                        cached.mask->load(std::memory_order_relaxed) & cached.bit);
                    return;
                }
            }
            return;
        }
        if (auto rumble = node->cast<ares::Node::Input::Rumble>()) {
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

// Cache button nodes under `parent` (a controller port or, for systems with
// built-in controls, the root node) into the given port bitmask.
static void cacheButtons(ares::Node::Object parent,
                         const SystemRegistry::SystemDef& def,
                         std::atomic<uint32_t>& mask) {
    for (auto& btn : parent->find<ares::Node::Input::Button>()) {
        auto it = def.buttons.find(std::string((const char*)btn->name()));
        if (it != def.buttons.end()) {
            g_ctx->inputCache.push_back({btn, &mask, it->second});
        }
    }
}

// ---------------------------------------------------------------------------
// C API implementation
// ---------------------------------------------------------------------------
extern "C" {

AresContext* ares_create(void) {
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
    ctx->system    = def;
    ctx->portsJson = SystemRegistry::staticPortsJson(*def);
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
    ctx->inputCache.clear();
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

    // Wire controllers and build the button cache. Ports JSON stays the
    // registry's static form — same names, same order as the node walk.
    std::atomic<uint32_t>* masks[2] = {
        &ctx->inputMaskPort1, &ctx->inputMaskPort2,
    };
    if (def->ports == 0) {
        // Built-in controls (e.g. Game Boy) — buttons live on the system node.
        cacheButtons(ctx->root, *def, ctx->inputMaskPort1);
    } else {
        for (int i = 0; i < def->ports && i < 2; i++) {
            auto portName = std::string("Controller Port ") + std::to_string(i + 1);
            auto port = ctx->root->find<ares::Node::Port>(portName.c_str());
            if (!port) continue;
            port->allocate(def->device);
            port->connect();
            cacheButtons(port, *def, *masks[i]);
        }
    }

    // Desktop applies its overscan setting to every screen on load
    // (emulator.cpp:137 → setOverscan scan, emulator.cpp:242-246). Cores
    // consult screen->overscan() per frame, so this takes effect immediately.
    for (auto& screen : ctx->root->find<ares::Node::Video::Screen>()) {
        screen->setOverscan(ctx->overscan);
    }
    ctx->systemLoaded = true;

    ctx->cartridgePak = built.pak;

    // Seed battery saves from disk before the boards read the pak at connect.
    ctx->savePrefix = save_prefix ? save_prefix : "";
    SaveIO::seed(ctx->cartridgePak, ctx->savePrefix);

    auto cartridgeSlot = ctx->root->find<ares::Node::Port>("Cartridge Slot");
    if (!cartridgeSlot) {
        unloadCore(ctx);
        return -1;
    }
    cartridgeSlot->allocate();
    cartridgeSlot->connect();

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
                    float gamma, bool color_bleed, bool interframe_blending,
                    bool overscan) {
    if (!ctx || !ctx->systemLoaded) return;
    ctx->overscan = overscan;
    for (auto& screen : ctx->root->find<ares::Node::Video::Screen>()) {
        screen->setLuminance((f64)luminance);
        screen->setSaturation((f64)saturation);
        screen->setGamma((f64)gamma);
        screen->setColorBleed(color_bleed);
        screen->setInterframeBlending(interframe_blending);
        screen->setOverscan(overscan);
    }
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
    if (ctx->paused.load(std::memory_order_relaxed)) return false;

    if (ctx->dynamicRateControl.load(std::memory_order_relaxed) &&
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
    if (!ctx) return;
    auto mask = static_cast<uint32_t>(bits);
    if (port == 1) ctx->inputMaskPort1.store(mask, std::memory_order_relaxed);
    else if (port == 2) ctx->inputMaskPort2.store(mask, std::memory_order_relaxed);
}

uint32_t ares_get_input(AresContext* ctx, int port) {
    if (!ctx) return 0;
    if (port == 1) return ctx->inputMaskPort1.load(std::memory_order_relaxed);
    if (port == 2) return ctx->inputMaskPort2.load(std::memory_order_relaxed);
    return 0;
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

const char* ares_get_ports_json(AresContext* ctx) {
    // Registry data — available from staging on, no booted core required.
    if (!ctx || !ctx->system) return "[]";
    return ctx->portsJson.c_str();
}

} // extern "C"
