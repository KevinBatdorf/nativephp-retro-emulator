// Bandai WonderSwan core module — self-registers via SystemRegistry::Registrar.
#include "system_registry.hpp"

#include <ws/ws.hpp>

#include "pak/multi_pak.hpp"

namespace {

using SystemRegistry::CartridgePak;
using SystemRegistry::SystemDef;

auto loadWs(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    // Desktop-ui wonderswan.cpp:72 — apu/ppu `accurate` is uninitialized until
    // this option lands, so skipping it is undefined behavior, not a default.
    ares::WonderSwan::option("Pixel Accuracy", false);
    return ares::WonderSwan::load(root, nall::string(loadName.c_str()));
}

auto memRead(uint32_t o) -> uint8_t { return (uint8_t)ares::WonderSwan::iram.read(o); }
auto memWrite(uint32_t o, uint8_t v) -> void { ares::WonderSwan::iram.write(o, v); }

auto systemPak(const SystemDef& def, const std::vector<uint8_t>&) -> std::shared_ptr<vfs::directory> {
    return MultiPak::makeSystemPak(def.id);
}

auto cartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
    auto built = MultiPak::makeCartridgePak("ws", rom, romSize);
    return {built.pak, built.title, built.region, built.error};
}

auto clearEntryPoints() -> void { ares::WonderSwan::Thread::EntryPoints().clear(); }

const SystemDef kDef = {
    .id            = "ws",
    .name          = "WonderSwan",
    .loadNameBase  = "[Bandai] WonderSwan",
    .regions       = {},   // region-free handheld
    .extensions    = {"ws"},
    .systemNode    = "WonderSwan",
    .cartridgeNode = "WonderSwan Cartridge",
    .device        = nullptr,   // controls live on the system node
    .ports         = 0,
    // Desktop wiring (wonderswan.cpp): X1-X4 are the d-pad diamond
    // (up/right/down/left), Y1-Y4 the second diamond, B south, A east.
    // Volume steps the master volume (ws/system/controls.cpp); Power is
    // excluded like fc/md's Reset — the Reset/Stop bridges cover it.
    .buttons       = {
        {"B", 1u << 0}, {"Y1", 1u << 1}, {"Start", 1u << 3},
        {"X1", 1u << 4}, {"X3", 1u << 5}, {"X4", 1u << 6}, {"X2", 1u << 7},
        {"A", 1u << 8}, {"Y2", 1u << 9}, {"Y3", 1u << 10}, {"Y4", 1u << 11},
        {"Volume", 1u << 12},
    },
    .biosRequired  = false,
    // Internal work RAM window (16 KiB on the mono WonderSwan).
    .memBase       = 0x0000u,
    .memSize       = 0x4000u,
    .load          = loadWs,
    .memRead       = memRead,
    .memWrite      = memWrite,
    .makeSystemPak = systemPak,
    .makeCartridgePak = cartridgePak,
    .makeSlotPak   = nullptr,
    .clearEntryPoints = clearEntryPoints,
};

const SystemRegistry::Registrar kRegistrar{&kDef};

} // namespace

// Link anchor for static builds — see the note in core_sfc.cpp.
extern "C" int retro_emulator_core_ws_link = 1;
