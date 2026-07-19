// NEC PC Engine / TurboGrafx-16 core module — self-registers via
// SystemRegistry::Registrar.
#include "system_registry.hpp"

#include <pce/pce.hpp>

#include "pak/multi_pak.hpp"

namespace {

using SystemRegistry::CartridgePak;
using SystemRegistry::SystemDef;

auto loadPce(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    // Desktop composes the name per region — the NTSC-U machine is the
    // "TurboGrafx 16" (pc-engine.cpp: region == "NTSC-J" ? "PC Engine" :
    // "TurboGrafx 16"). loadNameFor gave us "[NEC] PC Engine (REGION)";
    // swap the model in for the US variant.
    std::string name = loadName;
    if(name.find("NTSC-U") != std::string::npos) {
        name = "[NEC] TurboGrafx 16 (NTSC-U)";
    }
    // Desktop-ui pc-engine.cpp:101 — VDPBase::implementation is null until
    // this option lands (it force-selects the accurate VDP), so skipping it
    // segfaults on the first VDP call. The value is ignored upstream.
    ares::PCEngine::option("Pixel Accuracy", false);
    return ares::PCEngine::load(root, nall::string(name.c_str()));
}

auto memRead(uint32_t o) -> uint8_t { return (uint8_t)ares::PCEngine::cpu.ram[o]; }
auto memWrite(uint32_t o, uint8_t v) -> void { ares::PCEngine::cpu.ram[o] = v; }

auto systemPak(const SystemDef& def, const std::vector<uint8_t>&) -> std::shared_ptr<vfs::directory> {
    return MultiPak::makeSystemPak(def.id);
}

auto cartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
    auto built = MultiPak::makeCartridgePak("pce", rom, romSize);
    return {built.pak, built.title, built.region, built.error};
}

auto clearEntryPoints() -> void { ares::PCEngine::Thread::EntryPoints().clear(); }

const SystemDef kDef = {
    .id            = "pce",
    .name          = "PC Engine / TurboGrafx-16",
    .loadNameBase  = "[NEC] PC Engine",
    .regions       = {"NTSC-J", "NTSC-U"},
    .extensions    = {"pce"},
    .systemNode    = "PC Engine",      // "TurboGrafx 16" on NTSC-U; the
                                       // platform layers match the system
                                       // node by root pointer, not name
    .cartridgeNode = "PC Engine Card", // HuCards: "<system> Card", not
                                       // " Cartridge" (pce cartridge.cpp:11)
    .device        = "Gamepad",
    .ports         = 1,
    // Desktop wiring (pc-engine.cpp): II south, I east, Select, Run on start.
    .buttons       = {
        {"II", 1u << 0}, {"Select", 1u << 2}, {"Run", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"I", 1u << 8},
    },
    .biosRequired  = false,
    // HuC6280 work RAM (8 KiB), logically mapped at $2000 (physical page $F8).
    .memBase       = 0x2000u,
    .memSize       = 0x2000u,
    .load          = loadPce,
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
extern "C" int retro_emulator_core_pce_link = 1;
