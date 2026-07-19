// Sega SG-1000 core module — self-registers via SystemRegistry::Registrar.
#include "system_registry.hpp"

#include <sg/sg.hpp>

#include "pak/multi_pak.hpp"

namespace {

using SystemRegistry::CartridgePak;
using SystemRegistry::SystemDef;

auto loadSg(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    return ares::SG1000::load(root, nall::string(loadName.c_str()));
}

auto memRead(uint32_t o) -> uint8_t { return (uint8_t)ares::SG1000::cpu.ram[o]; }
auto memWrite(uint32_t o, uint8_t v) -> void { ares::SG1000::cpu.ram[o] = v; }

auto systemPak(const SystemDef& def) -> std::shared_ptr<vfs::directory> {
    return MultiPak::makeSystemPak(def.id);
}

auto cartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
    auto built = MultiPak::makeCartridgePak("sg", rom, romSize);
    return {built.pak, built.title, built.region, built.error};
}

auto clearEntryPoints() -> void { ares::SG1000::Thread::EntryPoints().clear(); }

const SystemDef kDef = {
    .id            = "sg",
    .name          = "Sega SG-1000",
    .loadNameBase  = "[Sega] SG-1000",
    .regions       = {"NTSC", "PAL"},
    .extensions    = {"sg1000", "sg"},
    .systemNode    = "SG-1000",
    .cartridgeNode = "SG-1000 Cartridge",
    .device        = "Gamepad",
    .ports         = 2,
    // Desktop wiring (sg-1000.cpp): 1 on face south, 2 on face east.
    .buttons       = {
        {"1", 1u << 0}, {"2", 1u << 8},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
    },
    .biosRequired  = false,
    .memBase       = 0xC000u,
    .memSize       = 0x400u,   // 1 KiB (SC-3000 variants map 2 KiB)
    .load          = loadSg,
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
extern "C" int retro_emulator_core_sg_link = 1;
