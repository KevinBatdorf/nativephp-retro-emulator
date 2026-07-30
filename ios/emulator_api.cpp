// C API marshalling over the engine-neutral EmulatorHost (native/host/) —
// the emulator logic that used to live here moved behind the backend seam.
// This file is Swift-facing plumbing only: pointer/string conversion and the
// static-link anchors that force the compiled cores (and the ares backend)
// into the image.
#include "emulator_api.h"

#include "host/backend_registry.hpp"
#include "host/emulator_host.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// Opaque context handed to Swift — wraps the host so emu_reset can restore
// factory state while the pointer the host layers stored stays valid.
struct EmuContext {
    EmuHost::EmulatorHost host;
};

static EmuContext* g_ctx = nullptr;

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

EmuContext* emu_create(void) {
    // Forces the statically-linked core objects (and their Registrars) plus
    // the ares backend into the image — see core_link.cpp.
    (void)retro_emulator_static_cores();
    if (!g_ctx) {
        g_ctx = new EmuContext{};
    }
    return g_ctx;
}

void emu_destroy(EmuContext* ctx) {
    if (!ctx || ctx != g_ctx) return;
    delete g_ctx;   // the host destructor unloads any running game
    g_ctx = nullptr;
}

void emu_reset(EmuContext* ctx) {
    if (!ctx || ctx != g_ctx) return;
    // Factory state in place — matches Android's destroy()+init() cycle
    // while keeping the pointer the host (audio/input) stored valid.
    ctx->host.reset();
}

const char* emu_supported_systems(void) {
    static thread_local std::string ids;
    ids = joinCsv(EmuHost::Backends::availableSystems());
    return ids.c_str();
}

bool emu_load_system(EmuContext* ctx, const char* system_id, const char* bios_path,
                     const char* backend) {
    if (!ctx || !system_id) return false;
    return ctx->host.stageSystem(system_id, bios_path ? bios_path : "",
                                 backend ? backend : "");
}

const char* emu_get_backend_name(EmuContext* ctx) {
    static thread_local std::string name;
    name = ctx ? ctx->host.backendName() : "";
    return name.c_str();
}

const char* emu_get_backends_json(void) {
    static thread_local std::string json;
    json = EmuHost::EmulatorHost::backendsJson();
    return json.c_str();
}

bool emu_video_settings_supported(EmuContext* ctx) {
    return ctx && ctx->host.videoSettingsSupported();
}

bool emu_toggle_supported(EmuContext* ctx, const char* key) {
    return ctx && key && ctx->host.toggleSupported(key);
}

const char* emu_set_engine_option(EmuContext* ctx, const char* key,
                                  const char* value, bool staged) {
    static thread_local std::string error;
    if (!ctx || !key || !value) {
        error = "emulator not initialized";
    } else {
        error = ctx->host.setEngineOption(key, value, staged);
    }
    return error.c_str();
}

const char* emu_get_engine_options_json(EmuContext* ctx) {
    static thread_local std::string json;
    json = ctx ? ctx->host.engineOptionsJson() : "[]";
    return json.c_str();
}

int emu_load_rom(EmuContext* ctx, const uint8_t* rom, size_t rom_size,
                  const char* save_prefix,
                  const char* region_override, const char* preferred_regions) {
    if (!ctx) return 0;
    return ctx->host.loadRom(rom, rom_size,
                             save_prefix ? save_prefix : "",
                             region_override ? region_override : "",
                             preferred_regions ? preferred_regions : "");
}

const char* emu_system_extensions(EmuContext*, const char* system_id) {
    static thread_local std::string exts;
    exts = joinCsv(EmuHost::EmulatorHost::systemExtensionsFor(system_id ? system_id : ""));
    return exts.c_str();
}

bool emu_add_cheat(EmuContext* ctx, const char* code) {
    if (!ctx || !code) return false;
    return ctx->host.addCheat(code);
}

bool emu_remove_cheat(EmuContext* ctx, const char* code) {
    if (!ctx || !code) return false;
    return ctx->host.removeCheat(code);
}

void emu_clear_cheats(EmuContext* ctx) {
    if (ctx) ctx->host.clearCheats();
}

void emu_set_audio(EmuContext* ctx, float volume, float balance) {
    if (ctx) ctx->host.setAudio(volume, balance);
}

void emu_set_video(EmuContext* ctx, float luminance, float saturation,
                    float gamma, bool color_bleed, bool overscan) {
    if (ctx) ctx->host.setVideo(luminance, saturation, gamma, color_bleed, overscan);
}

void emu_set_core_boolean(EmuContext* ctx, const char* key, bool value) {
    if (!ctx || !key) return;
    ctx->host.setCoreBoolean(key, value);
}

int emu_get_core_boolean(EmuContext* ctx, const char* key) {
    if (!ctx || !key) return -1;
    return ctx->host.coreBoolean(key);
}

double emu_get_refresh_rate_hint(EmuContext* ctx) {
    return ctx ? ctx->host.refreshRateHint() : 0.0;
}

void emu_get_video_geometry(EmuContext* ctx, double out[7]) {
    out[0] = 0; out[1] = 0; out[2] = 1; out[3] = 1;
    out[4] = 1; out[5] = 1; out[6] = 0;
    if (ctx) ctx->host.videoGeometry(out);
}

bool emu_flush_saves(EmuContext* ctx) {
    return ctx ? ctx->host.flushSaves() : false;
}

bool emu_tick(EmuContext* ctx) {
    return ctx ? ctx->host.tick() : false;
}

void emu_configure_rewind(EmuContext* ctx, bool enabled, int buffer_seconds) {
    if (ctx) ctx->host.configureRewind(enabled, buffer_seconds);
}

int emu_toggle_rewind(EmuContext* ctx) {
    return ctx ? ctx->host.toggleRewind() : -1;
}

void emu_set_run_ahead(EmuContext* ctx, bool enabled) {
    if (ctx) ctx->host.setRunAhead(enabled);
}

void emu_set_fast_forward(EmuContext* ctx, bool active) {
    if (ctx) ctx->host.setFastForward(active);
}

void emu_set_dynamic_rate_control(EmuContext* ctx, bool enabled) {
    if (ctx) ctx->host.setDynamicRateControl(enabled);
}

void emu_set_rumble_enabled(EmuContext* ctx, bool enabled) {
    if (ctx) ctx->host.setRumbleEnabled(enabled);
}

uint32_t emu_get_rumble_state(EmuContext* ctx) {
    return ctx ? ctx->host.rumbleState() : 0;
}

void emu_set_input(EmuContext* ctx, int port, uint32_t bits) {
    if (ctx) ctx->host.setInput(port, bits);
}

uint32_t emu_get_input(EmuContext* ctx, int port) {
    return ctx ? ctx->host.combinedInput(port) : 0;
}

void emu_stage_slot(EmuContext* ctx, int index, const uint8_t* rom, size_t rom_size) {
    if (ctx) ctx->host.stageSlot(index, rom, rom_size);
}

bool emu_is_slot_connected(EmuContext* ctx, int index) {
    return ctx ? ctx->host.isSlotConnected(index) : false;
}

void emu_stage_boot_option(EmuContext* ctx, const char* name, const char* value) {
    if (!ctx || !name || !value) return;
    ctx->host.stageBootOption(name, value);
}

const char* emu_get_boot_option(EmuContext* ctx, const char* name) {
    if (!ctx || !name) return "";
    static thread_local std::string value;
    value = ctx->host.readBootOption(name);
    return value.c_str();
}

const char* emu_connect_device(EmuContext* ctx, const char* system_id,
                                int port, const char* device) {
    if (!ctx) return statusRet("SYSTEM_NOT_LOADED");
    return statusRet(ctx->host.connectDevice(system_id ? system_id : "", port,
                                             device ? device : ""));
}

int emu_device_ports(EmuContext* ctx, const char* system_id,
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

const char* emu_press_button(EmuContext* ctx, int port,
                              const char* name, bool down) {
    if (!ctx) return statusRet("SYSTEM_NOT_LOADED");
    return statusRet(ctx->host.pressButton(port, name ? name : "", down));
}

const char* emu_set_axis(EmuContext* ctx, int port, const char* name, int value) {
    if (!ctx) return statusRet("SYSTEM_NOT_LOADED");
    return statusRet(ctx->host.setAxis(port, name ? name : "", value));
}

const char* emu_aim_at(EmuContext* ctx, int port, float nx, float ny) {
    if (!ctx) return statusRet("SYSTEM_NOT_LOADED");
    return statusRet(ctx->host.aimAt(port, nx, ny));
}

const char* emu_set_input_mapping(EmuContext* ctx, int port,
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

int emu_get_button_bit(EmuContext* ctx, int port, const char* name) {
    if (!ctx || !name) return -1;
    return ctx->host.getButtonBit(port, name);
}

int emu_get_axis_accum(EmuContext* ctx, int port, const char* name) {
    if (!ctx || !name) return 0;
    return ctx->host.getAxisAccum(port, name);
}

bool emu_get_frame(EmuContext* ctx,
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

size_t emu_read_audio(EmuContext* ctx, float* out, size_t capacity) {
    return ctx ? ctx->host.readAudio(out, capacity) : 0;
}

void emu_pause(EmuContext* ctx) {
    if (ctx) ctx->host.pause();
}

void emu_resume(EmuContext* ctx) {
    if (ctx) ctx->host.resume();
}

bool emu_state_save(EmuContext* ctx, const char* path) {
    if (!ctx || !path) return false;
    return ctx->host.stateSave(path);
}

bool emu_state_load(EmuContext* ctx, const char* path) {
    if (!ctx || !path) return false;
    return ctx->host.stateLoad(path);
}

int emu_read_memory(EmuContext* ctx, uint32_t address, uint8_t* out, int length) {
    if (!ctx || !out || length <= 0) return -1;
    return ctx->host.readMemory(address, out, (uint32_t)length);
}

void emu_write_memory(EmuContext* ctx, uint32_t address,
                       const uint8_t* bytes, int length)
{
    if (!ctx || !bytes || length <= 0) return;
    ctx->host.writeMemory(address, bytes, (uint32_t)length);
}

const char* emu_get_region(EmuContext* ctx) {
    if (!ctx) return "";
    static thread_local std::string region;
    region = ctx->host.region();
    return region.c_str();
}

/** Buttons held on a port, comma-joined. Mirrors the Android JNI function. */
const char* emu_get_pressed_buttons(EmuContext* ctx, int port) {
    static thread_local std::string out;
    out = ctx ? ctx->host.pressedButtons(port) : "";
    return out.c_str();
}

const char* emu_get_ports_json(EmuContext* ctx) {
    // Catalog data + registrations — available from staging on, no booted
    // core required. Thread-local storage, same contract as other strings.
    if (!ctx) return "[]";
    static thread_local std::string json;
    json = ctx->host.portsJson();
    return json.c_str();
}

} // extern "C"
