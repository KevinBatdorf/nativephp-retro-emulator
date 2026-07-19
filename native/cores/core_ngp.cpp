// SNK Neo Geo Pocket core module — self-registers via SystemRegistry::Registrar.
//
// biosRequired: the NGP BIOS is copyrighted SNK firmware that never ships with
// this plugin — devs supply their own dump via the LoadSystem config's
// biosPath (desktop-ui neo-geo-pocket.cpp gates load the same way).
#include "system_registry.hpp"

#include <ngp/ngp.hpp>

#include "pak/multi_pak.hpp"

namespace {

using SystemRegistry::CartridgePak;
using SystemRegistry::SystemDef;

auto loadNgp(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    return ares::NeoGeoPocket::load(root, nall::string(loadName.c_str()));
}

// 12KB CPU work RAM — the console's battery-backed main memory.
auto memRead(uint32_t o) -> uint8_t { return (uint8_t)ares::NeoGeoPocket::cpu.ram[o]; }
auto memWrite(uint32_t o, uint8_t v) -> void { ares::NeoGeoPocket::cpu.ram[o] = v; }

auto systemPak(const SystemDef& def, const std::vector<uint8_t>& bios) -> std::shared_ptr<vfs::directory> {
    return MultiPak::makeSystemPak(def.id, bios);
}

auto cartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
    auto built = MultiPak::makeCartridgePak("ngp", rom, romSize);
    return {built.pak, built.title, built.region, built.error};
}

auto clearEntryPoints() -> void { ares::NeoGeoPocket::Thread::EntryPoints().clear(); }

const SystemDef kDef = {
    .id            = "ngp",
    .name          = "Neo Geo Pocket",
    .loadNameBase  = "[SNK] Neo Geo Pocket",
    .regions       = {},   // region-free handheld
    .extensions    = {"ngp"},
    .systemNode    = "Neo Geo Pocket",
    .cartridgeNode = "Neo Geo Pocket Cartridge",
    .device        = nullptr,   // controls live on the system node
    .ports         = 0,
    // Desktop wiring (neo-geo-pocket.cpp): A south, B east; Option is the
    // console's start-equivalent; Power and Debugger are real inputs games
    // and the BIOS read, so desktop keeps them pressable.
    .buttons       = {
        {"A", 1u << 0}, {"Option", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"B", 1u << 8}, {"Power", 1u << 10}, {"Debugger", 1u << 11},
    },
    .biosRequired  = true,
    .memBase       = 0x4000u,   // CPU RAM window (tlcs900h address space)
    .memSize       = 0x3000u,
    .load          = loadNgp,
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
extern "C" int retro_emulator_core_ngp_link = 1;
