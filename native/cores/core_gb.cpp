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

// The patched gb/apu/sequencer.cpp reads this; false is upstream's mixer.
auto setOption(const std::string& name, const std::string& value) -> bool {
    if (name != "rawAudio") return false;
    ares::GameBoy::nativephpRemovePedestal = !(value == "true" || value == "1");
    return true;
}

auto getOption(const std::string& name) -> std::string {
    if (name != "rawAudio") return {};
    return ares::GameBoy::nativephpRemovePedestal ? "false" : "true";
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
    .loadNameBase  = "[Nintendo] Game Boy",
    .cartridgeNode = "Game Boy Cartridge",
    .load          = loadGb,
    .setOption     = setOption,
    .getOption     = getOption,
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
    .loadNameBase  = "[Nintendo] Game Boy Color",
    .cartridgeNode = "Game Boy Color Cartridge",
    .load          = loadGb,
    .setOption     = setOption,
    .getOption     = getOption,
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
