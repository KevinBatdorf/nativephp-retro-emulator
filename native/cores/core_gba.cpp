// Game Boy Advance core module — self-registers via SystemRegistry::Registrar.
//
// biosRequired: the GBA BIOS is copyrighted commercial firmware that never
// ships with this plugin — devs supply their own dump via the LoadSystem
// config's biosPath (desktop-ui game-boy-advance.cpp gates load the same way).
#include "system_registry.hpp"

#include <gba/gba.hpp>

#include "pak/multi_pak.hpp"

namespace {

using SystemRegistry::CartridgePak;
using SystemRegistry::SystemDef;

auto loadGba(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    return ares::GameBoyAdvance::load(root, nall::string(loadName.c_str()));
}

// 256KB EWRAM at 0x02000000 — the general-purpose work RAM.
auto memRead(uint32_t o) -> uint8_t { return (uint8_t)ares::GameBoyAdvance::cpu.ewram[o]; }
auto memWrite(uint32_t o, uint8_t v) -> void { ares::GameBoyAdvance::cpu.ewram[o] = v; }

auto systemPak(const SystemDef& def, const std::vector<uint8_t>& bios) -> std::shared_ptr<vfs::directory> {
    return MultiPak::makeSystemPak(def.id, bios);
}

auto cartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
    auto built = MultiPak::makeCartridgePak("gba", rom, romSize);
    return {built.pak, built.title, built.region, built.error};
}

auto clearEntryPoints() -> void { ares::GameBoyAdvance::Thread::EntryPoints().clear(); }

const SystemDef kDef = {
    .id            = "gba",
    .name          = "Game Boy Advance",
    .loadNameBase  = "[Nintendo] Game Boy Advance",
    .regions       = {},   // region-free handheld
    .extensions    = {"gba"},
    .systemNode    = "Game Boy Advance",
    .cartridgeNode = "Game Boy Advance Cartridge",
    .device        = nullptr,   // controls live on the system node
    .ports         = 0,
    .buttons       = {
        {"B", 1u << 0}, {"Select", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"A", 1u << 8}, {"L", 1u << 10}, {"R", 1u << 11},
    },
    .biosRequired  = true,
    .memBase       = 0x02000000u,
    .memSize       = 0x40000u,
    .load          = loadGba,
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
extern "C" int retro_emulator_core_gba_link = 1;
