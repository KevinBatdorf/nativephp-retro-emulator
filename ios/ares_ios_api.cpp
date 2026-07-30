// C API marshalling over the engine-neutral EmulatorHost (native/host/) —
// the emulator logic that used to live here moved behind the backend seam.
// This file is Swift-facing plumbing only: pointer/string conversion and the
// static-link anchors that force the compiled cores (and the ares backend)
// into the image.
#include "ares_ios_api.h"

#include "host/backend_registry.hpp"
#include "host/emulator_host.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// Opaque context handed to Swift — wraps the host so ares_reset can restore
// factory state while the pointer the host layers stored stays valid.
struct AresContext {
    EmuHost::EmulatorHost host;
};

static AresContext* g_ctx = nullptr;

// Status-string returns share one thread-local buffer (copied by the caller
// before the next call on the same thread, per the header contract).
static const char* statusRet(const std::string& s) {
    static thread_local std::string buf;
    buf = s;
    return buf.c_str();
}

static std::string joinCsv(const std::vector<std::string>& items) {
    std::string out;
    for (auto& item : items) {
        if (!out.empty()) out += ",";
        out += item;
    }
    return out;
}

extern "C" {

extern "C" int retro_emulator_static_cores();

AresContext* ares_create(void) {
    // Forces the statically-linked core objects (and their Registrars) plus
    // the ares backend into the image — see core_link.cpp.
    (void)retro_emulator_static_cores();
    if (!g_ctx) {
        g_ctx = new AresContext{};
    }
    return g_ctx;
}

void ares_destroy(AresContext* ctx) {
    if (!ctx || ctx != g_ctx) return;
    delete g_ctx;   // the host destructor unloads any running game
    g_ctx = nullptr;
}

void ares_reset(AresContext* ctx) {
    if (!ctx || ctx != g_ctx) return;
    // Factory state in place — matches Android's destroy()+init() cycle
    // while keeping the pointer the host (audio/input) stored valid.
    ctx->host.reset();
}

const char* ares_supported_systems(void) {
    static thread_local std::string ids;
    ids = joinCsv(EmuHost::Backends::availableSystems());
    return ids.c_str();
}

bool ares_load_system(AresContext* ctx, const char* system_id, const char* bios_path) {
    if (!ctx || !system_id) return false;
    return ctx->host.stageSystem(system_id, bios_path ? bios_path : "");
}

int ares_load_rom(AresContext* ctx, const uint8_t* rom, size_t rom_size,
                  const char* save_prefix,
                  const char* region_override, const char* preferred_regions) {
    if (!ctx) return 0;
    return ctx->host.loadRom(rom, rom_size,
                             save_prefix ? save_prefix : "",
                             region_override ? region_override : "",
                             preferred_regions ? preferred_regions : "");
}

const char* ares_system_extensions(AresContext*, const char* system_id) {
    static thread_local std::string exts;
    exts = joinCsv(EmuHost::EmulatorHost::systemExtensionsFor(system_id ? system_id : ""));
    return exts.c_str();
}

bool ares_add_cheat(AresContext* ctx, const char* code) {
    if (!ctx || !code) return false;
    return ctx->host.addCheat(code);
}

bool ares_remove_cheat(AresContext* ctx, const char* code) {
    if (!ctx || !code) return false;
    return ctx->host.removeCheat(code);
}

void ares_clear_cheats(AresContext* ctx) {
    if (ctx) ctx->host.clearCheats();
}

void ares_set_audio(AresContext* ctx, float volume, float balance) {
    if (ctx) ctx->host.setAudio(volume, balance);
}

void ares_set_video(AresContext* ctx, float luminance, float saturation,
                    float gamma, bool color_bleed, bool overscan) {
    if (ctx) ctx->host.setVideo(luminance, saturation, gamma, color_bleed, overscan);
}

void ares_set_core_boolean(AresContext* ctx, const char* key, bool value) {
    if (!ctx || !key) return;
    ctx->host.setCoreBoolean(key, value);
}

int ares_get_core_boolean(AresContext* ctx, const char* key) {
    if (!ctx || !key) return -1;
    return ctx->host.coreBoolean(key);
}

double ares_get_refresh_rate_hint(AresContext* ctx) {
    return ctx ? ctx->host.refreshRateHint() : 0.0;
}

void ares_get_video_geometry(AresContext* ctx, double out[7]) {
    out[0] = 0; out[1] = 0; out[2] = 1; out[3] = 1;
    out[4] = 1; out[5] = 1; out[6] = 0;
    if (ctx) ctx->host.videoGeometry(out);
}

bool ares_flush_saves(AresContext* ctx) {
    return ctx ? ctx->host.flushSaves() : false;
}

bool ares_tick(AresContext* ctx) {
    return ctx ? ctx->host.tick() : false;
}

void ares_configure_rewind(AresContext* ctx, bool enabled, int buffer_seconds) {
    if (ctx) ctx->host.configureRewind(enabled, buffer_seconds);
}

int ares_toggle_rewind(AresContext* ctx) {
    return ctx ? ctx->host.toggleRewind() : -1;
}

void ares_set_run_ahead(AresContext* ctx, bool enabled) {
    if (ctx) ctx->host.setRunAhead(enabled);
}

void ares_set_fast_forward(AresContext* ctx, bool active) {
    if (ctx) ctx->host.setFastForward(active);
}

void ares_set_dynamic_rate_control(AresContext* ctx, bool enabled) {
    if (ctx) ctx->host.setDynamicRateControl(enabled);
}

void ares_set_rumble_enabled(AresContext* ctx, bool enabled) {
    if (ctx) ctx->host.setRumbleEnabled(enabled);
}

uint32_t ares_get_rumble_state(AresContext* ctx) {
    return ctx ? ctx->host.rumbleState() : 0;
}

void ares_set_input(AresContext* ctx, int port, uint32_t bits) {
    if (ctx) ctx->host.setInput(port, bits);
}

uint32_t ares_get_input(AresContext* ctx, int port) {
    return ctx ? ctx->host.combinedInput(port) : 0;
}

void ares_stage_slot(AresContext* ctx, int index, const uint8_t* rom, size_t rom_size) {
    if (ctx) ctx->host.stageSlot(index, rom, rom_size);
}

bool ares_is_slot_connected(AresContext* ctx, int index) {
    return ctx ? ctx->host.isSlotConnected(index) : false;
}

void ares_stage_boot_option(AresContext* ctx, const char* name, const char* value) {
    if (!ctx || !name || !value) return;
    ctx->host.stageBootOption(name, value);
}

const char* ares_get_boot_option(AresContext* ctx, const char* name) {
    if (!ctx || !name) return "";
    static thread_local std::string value;
    value = ctx->host.readBootOption(name);
    return value.c_str();
}

const char* ares_connect_device(AresContext* ctx, const char* system_id,
                                int port, const char* device) {
    if (!ctx) return statusRet("SYSTEM_NOT_LOADED");
    return statusRet(ctx->host.connectDevice(system_id ? system_id : "", port,
                                             device ? device : ""));
}

int ares_device_ports(AresContext* ctx, const char* system_id,
                      int physical, int* out, int capacity) {
    if (!ctx || !out || capacity <= 0) return 0;
    auto ports = ctx->host.devicePorts(system_id ? system_id : "", physical);
    int count = 0;
    for (int port : ports) {
        if (count >= capacity) break;
        out[count++] = port;
    }
    return count;
}

const char* ares_press_button(AresContext* ctx, int port,
                              const char* name, bool down) {
    if (!ctx) return statusRet("SYSTEM_NOT_LOADED");
    return statusRet(ctx->host.pressButton(port, name ? name : "", down));
}

const char* ares_set_axis(AresContext* ctx, int port, const char* name, int value) {
    if (!ctx) return statusRet("SYSTEM_NOT_LOADED");
    return statusRet(ctx->host.setAxis(port, name ? name : "", value));
}

const char* ares_aim_at(AresContext* ctx, int port, float nx, float ny) {
    if (!ctx) return statusRet("SYSTEM_NOT_LOADED");
    return statusRet(ctx->host.aimAt(port, nx, ny));
}

const char* ares_set_input_mapping(AresContext* ctx, int port,
                                   const char* const* emulated,
                                   const char* const* source, int count) {
    // Staged-system gate precedes shape validation: callers key on
    // SYSTEM_NOT_LOADED to distinguish "nothing to remap against" from a
    // malformed pair list.
    if (!ctx || !ctx->host.systemStaged()) return statusRet("SYSTEM_NOT_LOADED");
    if (count > 0 && (!emulated || !source)) return statusRet("INVALID_PARAMETERS");

    std::vector<std::string> emulatedNames, sourceNames;
    emulatedNames.reserve((size_t)std::max(count, 0));
    sourceNames.reserve((size_t)std::max(count, 0));
    for (int i = 0; i < count; i++) {
        emulatedNames.emplace_back(emulated[i] ? emulated[i] : "");
        sourceNames.emplace_back(source[i] ? source[i] : "");
    }
    return statusRet(ctx->host.setInputMapping(port, emulatedNames, sourceNames));
}

int ares_get_button_bit(AresContext* ctx, int port, const char* name) {
    if (!ctx || !name) return -1;
    return ctx->host.getButtonBit(port, name);
}

int ares_get_axis_accum(AresContext* ctx, int port, const char* name) {
    if (!ctx || !name) return 0;
    return ctx->host.getAxisAccum(port, name);
}

bool ares_get_frame(AresContext* ctx,
                    uint32_t* out_buf, size_t buf_capacity,
                    uint32_t* out_width, uint32_t* out_height)
{
    if (!ctx) {
        if (out_width)  *out_width  = 0;
        if (out_height) *out_height = 0;
        return false;
    }
    return ctx->host.copyLatestFrame(out_buf, buf_capacity, out_width, out_height);
}

size_t ares_read_audio(AresContext* ctx, float* out, size_t capacity) {
    return ctx ? ctx->host.readAudio(out, capacity) : 0;
}

void ares_pause(AresContext* ctx) {
    if (ctx) ctx->host.pause();
}

void ares_resume(AresContext* ctx) {
    if (ctx) ctx->host.resume();
}

bool ares_state_save(AresContext* ctx, const char* path) {
    if (!ctx || !path) return false;
    return ctx->host.stateSave(path);
}

bool ares_state_load(AresContext* ctx, const char* path) {
    if (!ctx || !path) return false;
    return ctx->host.stateLoad(path);
}

int ares_read_memory(AresContext* ctx, uint32_t address, uint8_t* out, int length) {
    if (!ctx || !out || length <= 0) return -1;
    return ctx->host.readMemory(address, out, (uint32_t)length);
}

void ares_write_memory(AresContext* ctx, uint32_t address,
                       const uint8_t* bytes, int length)
{
    if (!ctx || !bytes || length <= 0) return;
    ctx->host.writeMemory(address, bytes, (uint32_t)length);
}

const char* ares_get_region(AresContext* ctx) {
    if (!ctx) return "";
    static thread_local std::string region;
    region = ctx->host.region();
    return region.c_str();
}

/** Buttons held on a port, comma-joined. Mirrors the Android JNI function. */
const char* ares_get_pressed_buttons(AresContext* ctx, int port) {
    static thread_local std::string out;
    out = ctx ? ctx->host.pressedButtons(port) : "";
    return out.c_str();
}

const char* ares_get_ports_json(AresContext* ctx) {
    // Catalog data + registrations — available from staging on, no booted
    // core required. Thread-local storage, same contract as other strings.
    if (!ctx) return "[]";
    static thread_local std::string json;
    json = ctx->host.portsJson();
    return json.c_str();
}

} // extern "C"
