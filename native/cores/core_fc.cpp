// NES / Famicom core module — self-registers via SystemRegistry::Registrar.
#include "system_registry.hpp"

#include <fc/fc.hpp>

#include "pak/multi_pak.hpp"

namespace {

using SystemRegistry::CartridgePak;
using SystemRegistry::SystemDef;

auto loadFc(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    return ares::Famicom::load(root, nall::string(loadName.c_str()));
}

auto memRead(uint32_t o) -> uint8_t { return (uint8_t)ares::Famicom::cpu.ram[o]; }
auto memWrite(uint32_t o, uint8_t v) -> void { ares::Famicom::cpu.ram[o] = v; }

auto systemPak(const SystemDef& def) -> std::shared_ptr<vfs::directory> {
    return MultiPak::makeSystemPak(def.id);
}

auto cartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
    auto built = MultiPak::makeCartridgePak("fc", rom, romSize);
    return {built.pak, built.title, built.region, built.error};
}

auto clearEntryPoints() -> void { ares::Famicom::Thread::EntryPoints().clear(); }

const SystemDef kDef = {
    .id            = "fc",
    .name          = "NES / Famicom",
    .loadNameBase  = "[Nintendo] Famicom",
    .regions       = {"NTSC-J", "NTSC-U", "PAL"},
    .extensions    = {"fc", "nes", "unf", "unif", "unh"},
    .systemNode    = "Famicom",
    .cartridgeNode = "Famicom Cartridge",
    .device        = "Gamepad",
    .ports         = 2,
    .buttons       = {
        {"B", 1u << 0}, {"Select", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"A", 1u << 8},
    },
    .biosRequired  = false,
    .memBase       = 0x0000u,
    .memSize       = 0x800u,
    .load          = loadFc,
    .memRead       = memRead,
    .memWrite      = memWrite,
    .makeSystemPak = systemPak,
    .makeCartridgePak = cartridgePak,
    .makeSlotPak   = nullptr,
    .clearEntryPoints = clearEntryPoints,
};

const SystemRegistry::Registrar kRegistrar{&kDef};

} // namespace

// Link anchor for static builds — a self-registering TU is referenced by
// nothing, so archives would drop it; the iOS aggregator names this symbol
// to force the object (and its Registrar) into the image.
extern "C" int retro_emulator_core_fc_link = 1;
