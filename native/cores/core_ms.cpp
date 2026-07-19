// Sega Master System core module — self-registers via SystemRegistry::Registrar.
#include "system_registry.hpp"

#include <ms/ms.hpp>

#include "pak/multi_pak.hpp"

namespace {

using SystemRegistry::CartridgePak;
using SystemRegistry::SystemDef;

auto loadMs(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    return ares::MasterSystem::load(root, nall::string(loadName.c_str()));
}

auto memRead(uint32_t o) -> uint8_t { return (uint8_t)ares::MasterSystem::cpu.ram[o]; }
auto memWrite(uint32_t o, uint8_t v) -> void { ares::MasterSystem::cpu.ram[o] = v; }

auto systemPak(const SystemDef& def, const std::vector<uint8_t>& bios) -> std::shared_ptr<vfs::directory> {
    return MultiPak::makeSystemPak(def.id, bios);
}

auto cartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
    auto built = MultiPak::makeCartridgePak("ms", rom, romSize);
    return {built.pak, built.title, built.region, built.error};
}

auto clearEntryPoints() -> void { ares::MasterSystem::Thread::EntryPoints().clear(); }

const SystemDef kDef = {
    .id            = "ms",
    .name          = "Sega Master System",
    .loadNameBase  = "[Sega] Master System",
    // Desktop's regionID priority order (master-system.cpp:102-105).
    .regions       = {"NTSC-U", "NTSC-J", "PAL"},
    .extensions    = {"ms", "sms"},
    .systemNode    = "Master System",
    .cartridgeNode = "Master System Cartridge",
    .device        = "Gamepad",
    .ports         = 2,
    // Desktop wiring (master-system.cpp): 1 south, 2 east — same pad as SG-1000.
    .buttons       = {
        {"1", 1u << 0}, {"2", 1u << 8},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
    },
    // Pause lives on the console (ms/system/controls.cpp), not the pad; it
    // triggers the NMI games use for their pause menus. Rapid/Reset stay
    // excluded like fc/md's Reset — the Reset/Stop bridges cover them.
    .systemButtons = {
        {"Pause", 1u << 3},
    },
    // Desktop always plugs the FM Sound Unit into the Expansion Port
    // (master-system.cpp:133-136); games probe for it and opt in.
    .extraPorts    = {{"Expansion Port", "FM Sound Unit"}},
    // BIOS optional: carts boot directly; a dev-supplied biosPath is honored.
    .biosRequired  = false,
    .memBase       = 0xC000u,
    .memSize       = 0x2000u,
    .load          = loadMs,
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
extern "C" int retro_emulator_core_ms_link = 1;
