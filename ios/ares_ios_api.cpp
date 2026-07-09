#include "ares_ios_api.h"
#include "sfc_pak.hpp"

#include <ares/ares.hpp>
#include <sfc/sfc.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Emulator state — mirrors the Android EmulatorState.
// Video frames are stored in a heap buffer instead of a GL texture.
// ---------------------------------------------------------------------------
struct AresContext {
    std::shared_ptr<vfs::directory> systemPak;
    std::shared_ptr<vfs::directory> cartridgePak;

    ares::Node::System root;
    bool systemLoaded = false;
    bool romLoaded    = false;

    // Video — ARGB8888 pixels, written by the Platform::video() callback.
    std::vector<uint32_t> frameBuffer;
    uint32_t frameWidth  = 0;
    uint32_t frameHeight = 0;

    // Audio — mixed stereo floats, written by Platform::audio().
    std::mutex    audioMutex;
    std::vector<float> audioRingBuffer;
    std::vector<ares::Node::Audio::Stream> audioStreams;
    static constexpr size_t kAudioCap = 48000u;

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

    // ROM metadata (set once during ares_load_rom, read-only afterward).
    std::string romRegion;
    std::string portsJson;
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
        if (!g_ctx) return {};
        auto name = node->name();
        if (name == "Super Famicom")           return g_ctx->systemPak;
        if (name == "Super Famicom Cartridge") return g_ctx->cartridgePak;
        return {};
    }

    auto video(ares::Node::Video::Screen node,
               const u32* data, u32 pitch, u32 width, u32 height) -> void override {
        if (!g_ctx) return;
        g_ctx->frameWidth  = width;
        g_ctx->frameHeight = height;
        g_ctx->frameBuffer.resize((size_t)width * height);
        for (u32 y = 0; y < height; y++) {
            std::memcpy(&g_ctx->frameBuffer[y * width],
                        data + y * pitch, width * sizeof(u32));
        }
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
            std::lock_guard<std::mutex> lock(g_ctx->audioMutex);
            if (g_ctx->audioRingBuffer.size() < AresContext::kAudioCap) {
                g_ctx->audioRingBuffer.push_back(l);
                g_ctx->audioRingBuffer.push_back(r);
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
        }
    }
};

static IosPlatform* g_platform = nullptr;

// ---------------------------------------------------------------------------
// SNES button name → bitmask (matches iOS EmulatorInput bit positions).
// ---------------------------------------------------------------------------
static const std::unordered_map<std::string, uint32_t> kSnesButtons = {
    {"B",      1u <<  0}, {"Y",      1u <<  1},
    {"Select", 1u <<  2}, {"Start",  1u <<  3},
    {"Up",     1u <<  4}, {"Down",   1u <<  5},
    {"Left",   1u <<  6}, {"Right",  1u <<  7},
    {"A",      1u <<  8}, {"X",      1u <<  9},
    {"L",      1u << 10}, {"R",      1u << 11},
};

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
    if (g_ctx->systemLoaded) {
        g_ctx->root.reset();
    }
    ares::platform = nullptr;
    delete g_platform; g_platform = nullptr;
    delete g_ctx;      g_ctx      = nullptr;
}

bool ares_load_system(AresContext* ctx,
                      const uint8_t* ipl_rom, size_t ipl_size,
                      const uint8_t* boards_bml, size_t bml_size)
{
    if (!ctx) return false;
    if (ctx->systemLoaded) return true;
    if (!boards_bml || bml_size == 0) return false;

    ctx->systemPak = SfcPakBuilder::makeSystemPak(
        ipl_rom, ipl_size, boards_bml, bml_size);

    bool ok = ares::SuperFamicom::load(
        ctx->root, "[Nintendo] Super Famicom (NTSC)");
    if (!ok) {
        ctx->systemPak.reset();
        return false;
    }

    if (auto port = ctx->root->find<ares::Node::Port>("Controller Port 1")) {
        port->allocate("Gamepad");
        port->connect();
    }
    if (auto port = ctx->root->find<ares::Node::Port>("Controller Port 2")) {
        port->allocate("Gamepad");
        port->connect();
    }

    auto cachePortButtons = [&](const char* portName, std::atomic<uint32_t>& mask) {
        auto port = ctx->root->find<ares::Node::Port>(portName);
        if (!port) return;
        for (auto& btn : port->find<ares::Node::Input::Button>()) {
            auto it = kSnesButtons.find(std::string((const char*)btn->name()));
            if (it != kSnesButtons.end()) {
                ctx->inputCache.push_back({btn, &mask, it->second});
            }
        }
    };
    cachePortButtons("Controller Port 1", ctx->inputMaskPort1);
    cachePortButtons("Controller Port 2", ctx->inputMaskPort2);

    // Build ports JSON.
    {
        std::string json = "[";
        bool firstPort = true;
        const char* portNames[]  = {"Controller Port 1", "Controller Port 2"};
        const char* portLabels[] = {"1", "2"};
        for (int i = 0; i < 2; ++i) {
            auto port = ctx->root->find<ares::Node::Port>(portNames[i]);
            if (!port) continue;
            if (!firstPort) json += ",";
            firstPort = false;
            json += std::string("{\"port\":") + portLabels[i] + ",\"buttons\":[";
            bool firstBtn = true;
            for (auto& btn : port->find<ares::Node::Input::Button>()) {
                if (!firstBtn) json += ",";
                firstBtn = false;
                json += "\"";
                json += std::string((const char*)btn->name());
                json += "\"";
            }
            json += "]}";
        }
        json += "]";
        ctx->portsJson = json;
    }

    ctx->systemLoaded = true;
    return true;
}

bool ares_load_rom(AresContext* ctx, const uint8_t* rom, size_t rom_size) {
    if (!ctx || !ctx->systemLoaded || !rom || rom_size == 0) return false;

    auto info = SfcPakBuilder::detectHeader(rom, rom_size);
    if (info.board.empty()) return false;

    ctx->cartridgePak = SfcPakBuilder::makeCartridgePak(info, rom, rom_size);
    ctx->romRegion    = info.region;

    auto cartridgeSlot = ctx->root->find<ares::Node::Port>("Cartridge Slot");
    if (!cartridgeSlot) return false;

    cartridgeSlot->disconnect();
    cartridgeSlot->allocate();
    cartridgeSlot->connect();

    ctx->root->power(false);
    ctx->romLoaded = true;
    return true;
}

bool ares_tick(AresContext* ctx) {
    if (!ctx || !ctx->romLoaded) return false;
    if (ctx->paused.load(std::memory_order_relaxed)) return false;
    ares::SuperFamicom::system.run();
    return true;
}

void ares_set_input(AresContext* ctx, int port, uint32_t bits) {
    if (!ctx) return;
    auto mask = static_cast<uint32_t>(bits);
    if (port == 1) ctx->inputMaskPort1.store(mask, std::memory_order_relaxed);
    else if (port == 2) ctx->inputMaskPort2.store(mask, std::memory_order_relaxed);
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
    if (!ctx || !ctx->romLoaded || !out || length <= 0) return -1;
    uint32_t len = (uint32_t)length;
    if (address < 0x7E0000u || address + len > 0x800000u) return -1;
    uint32_t offset = address - 0x7E0000u;
    if (offset + len > 0x20000u) return -1;
    std::memcpy(out, &ares::SuperFamicom::cpu.wram[offset], len);
    return (int)len;
}

void ares_write_memory(AresContext* ctx, uint32_t address,
                       const uint8_t* bytes, int length)
{
    if (!ctx || !ctx->romLoaded || !bytes || length <= 0) return;
    uint32_t len = (uint32_t)length;
    if (address < 0x7E0000u || address + len > 0x800000u) return;
    uint32_t offset = address - 0x7E0000u;
    if (offset + len > 0x20000u) return;
    std::memcpy(&ares::SuperFamicom::cpu.wram[offset], bytes, len);
}

const char* ares_get_region(AresContext* ctx) {
    if (!ctx) return "";
    return ctx->romRegion.c_str();
}

const char* ares_get_ports_json(AresContext* ctx) {
    if (!ctx || !ctx->systemLoaded) return "[]";
    return ctx->portsJson.c_str();
}

} // extern "C"
