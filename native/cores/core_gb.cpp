// Game Boy core module — self-registers via SystemRegistry::Registrar.
#include "system_registry.hpp"

#include <gb/gb.hpp>

#include "pak/multi_pak.hpp"

namespace {

using SystemRegistry::CartridgePak;
using SystemRegistry::SystemDef;

auto loadGb(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    return ares::GameBoy::load(root, nall::string(loadName.c_str()));
}

auto memRead(uint32_t o) -> uint8_t { return (uint8_t)ares::GameBoy::cpu.wram[o]; }
auto memWrite(uint32_t o, uint8_t v) -> void { ares::GameBoy::cpu.wram[o] = v; }

auto systemPak(const SystemDef& def, const std::vector<uint8_t>&) -> std::shared_ptr<vfs::directory> {
    return MultiPak::makeSystemPak(def.id);
}

auto cartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
    auto built = MultiPak::makeCartridgePak("gb", rom, romSize);
    return {built.pak, built.title, built.region, built.error};
}

auto cartridgePakCgb(const uint8_t* rom, size_t romSize) -> CartridgePak {
    auto built = MultiPak::makeCartridgePak("gbc", rom, romSize);
    return {built.pak, built.title, built.region, built.error};
}

auto clearEntryPoints() -> void { ares::GameBoy::Thread::EntryPoints().clear(); }

const SystemDef kDef = {
    .id            = "gb",
    .name          = "Game Boy",
    .loadNameBase  = "[Nintendo] Game Boy",
    .regions       = {},   // region-free handheld
    .extensions    = {"gb"},
    .systemNode    = "Game Boy",
    .cartridgeNode = "Game Boy Cartridge",
    .device        = nullptr,   // controls live on the system node
    .ports         = 0,
    .buttons       = {
        {"B", 1u << 0}, {"Select", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"A", 1u << 8},
    },
    .biosRequired  = false,
    .memBase       = 0xC000u,
    .memSize       = 0x2000u,
    .load          = loadGb,
    .memRead       = memRead,
    .memWrite      = memWrite,
    .makeSystemPak = systemPak,
    .makeCartridgePak = cartridgePak,
    .makeSlotPak   = nullptr,
    .clearEntryPoints = clearEntryPoints,
};

// Game Boy Color — the same ares core booted as its CGB model (system.cpp:9
// "[Nintendo] Game Boy Color"); the pak carries the CGB boot ROM instead.
const SystemDef kDefCgb = {
    .id            = "gbc",
    .name          = "Game Boy Color",
    .loadNameBase  = "[Nintendo] Game Boy Color",
    .regions       = {},
    .extensions    = {"gbc"},
    .systemNode    = "Game Boy Color",
    .cartridgeNode = "Game Boy Color Cartridge",
    .device        = nullptr,
    .ports         = 0,
    .buttons       = {
        {"B", 1u << 0}, {"Select", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"A", 1u << 8},
    },
    .biosRequired  = false,
    .memBase       = 0xC000u,
    .memSize       = 0x2000u,
    .load          = loadGb,
    .memRead       = memRead,
    .memWrite      = memWrite,
    .makeSystemPak = systemPak,
    .makeCartridgePak = cartridgePakCgb,
    .makeSlotPak   = nullptr,
    .clearEntryPoints = clearEntryPoints,
};

const SystemRegistry::Registrar kRegistrar{&kDef};
const SystemRegistry::Registrar kRegistrarCgb{&kDefCgb};

} // namespace

// Link anchor for static builds — a self-registering TU is referenced by
// nothing, so archives would drop it; the iOS aggregator names this symbol
// to force the object (and its Registrar) into the image.
extern "C" int retro_emulator_core_gb_link = 1;
