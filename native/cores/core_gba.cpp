// Game Boy Advance core module — self-registers via SystemRegistry::Registrar.
//
// GBA boots on an embedded open BIOS (Cult-of-GBA, MIT — see multi_pak.cpp);
// a dev may still pass a real BIOS dump via the LoadSystem config's biosPath
// to override it for maximum accuracy.
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

auto setOption(const std::string& name, const std::string& value) -> bool {
    return ares::GameBoyAdvance::option(nall::string(name.c_str()), nall::string(value.c_str()));
}

auto getOption(const std::string& name) -> std::string {
    if (name == "Pixel Accuracy") return ares::GameBoyAdvance::ppu.accurate ? "true" : "false";
    return {};
}

const SystemDef kDef = {
    .id            = "gba",
    .loadNameBase  = "[Nintendo] Game Boy Advance",
    .cartridgeNode = "Game Boy Advance Cartridge",
    .load          = loadGba,
    .memRead       = memRead,
    .memWrite      = memWrite,
    .makeSystemPak = systemPak,
    .makeCartridgePak = cartridgePak,
    .makeSlotPak   = nullptr,
    .clearEntryPoints = clearEntryPoints,
    .setOption     = setOption,
    .getOption     = getOption,
};

const SystemRegistry::Registrar kRegistrar{&kDef};

} // namespace

// Link anchor for static builds — see the note in core_sfc.cpp.
extern "C" int retro_emulator_core_gba_link = 1;
