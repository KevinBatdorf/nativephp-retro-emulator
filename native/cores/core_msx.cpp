// Microsoft MSX core module — self-registers via SystemRegistry::Registrar.
//
// biosRequired: MSX main-ROM firmware is copyrighted (per-manufacturer) and
// never ships with this plugin — devs supply an image via the LoadSystem
// config's biosPath. The freely-licensed C-BIOS project works for most
// cartridge games. Cartridges only (mia's tape paths are out of scope); the
// MSX2 model is a separate registry decision, like WonderSwan Color.
#include "system_registry.hpp"

#include <msx/msx.hpp>

#include "pak/multi_pak.hpp"

namespace {

using SystemRegistry::CartridgePak;
using SystemRegistry::SystemDef;

auto loadMsx(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    return ares::MSX::load(root, nall::string(loadName.c_str()));
}

// 64KB main RAM on the MSX model.
auto memRead(uint32_t o) -> uint8_t { return (uint8_t)ares::MSX::cpu.ram[o]; }
auto memWrite(uint32_t o, uint8_t v) -> void { ares::MSX::cpu.ram[o] = v; }

auto systemPak(const SystemDef& def, const std::vector<uint8_t>& bios) -> std::shared_ptr<vfs::directory> {
    return MultiPak::makeSystemPak(def.id, bios);
}

auto cartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
    auto built = MultiPak::makeCartridgePak("msx", rom, romSize);
    return {built.pak, built.title, built.region, built.error};
}

auto clearEntryPoints() -> void { ares::MSX::Thread::EntryPoints().clear(); }

const SystemDef kDef = {
    .id            = "msx",
    .name          = "MSX",
    .loadNameBase  = "[Microsoft] MSX",
    .regions       = {"NTSC", "PAL"},
    .extensions    = {"rom", "mx1"},
    .systemNode    = "MSX",
    .cartridgeNode = "MSX Cartridge",
    .device        = "Gamepad",
    .ports         = 2,
    // Desktop wiring (msx.cpp): A south, B east.
    .buttons       = {
        {"A", 1u << 0},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"B", 1u << 8},
    },
    // Desktop always connects the Japanese keyboard (msx.cpp:92); many games
    // read keyboard rows even when played on the joystick.
    .extraPorts    = {{"Keyboard", "Japanese"}},
    .biosRequired  = true,
    .memBase       = 0xC000u,   // upper 16K of the 64K map is always RAM
    .memSize       = 0x4000u,
    .load          = loadMsx,
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
extern "C" int retro_emulator_core_msx_link = 1;
