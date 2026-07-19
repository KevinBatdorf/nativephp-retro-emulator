// SNES / Super Famicom core module — self-registers via SystemRegistry::Registrar,
// so bundling this translation unit (statically on iOS, as a dlopen'd library
// in the modular Android build) is what makes the system exist.
#include "system_registry.hpp"

#include <sfc/sfc.hpp>

#include "pak/sfc_pak.hpp"
#include "embedded_firmware.hpp"

namespace {

using SystemRegistry::CartridgePak;
using SystemRegistry::SystemDef;

auto loadSfc(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    // PPUBase dispatches through an implementation pointer that is NULL until
    // setAccurate() picks one — the desktop frontend always sets the
    // "Pixel Accuracy" option before load; without this, load crashes in
    // bus.reset() → ppu.map(). false = the performance PPU.
    ares::SuperFamicom::ppu.setAccurate(false);
    return ares::SuperFamicom::load(root, nall::string(loadName.c_str()));
}

// Offsets are relative to memBase; bounds are enforced by the caller against
// memSize before these are invoked.
auto memRead(uint32_t o) -> uint8_t { return ares::SuperFamicom::cpu.wram[o]; }
auto memWrite(uint32_t o, uint8_t v) -> void { ares::SuperFamicom::cpu.wram[o] = v; }

auto systemPak(const SystemDef&, const std::vector<uint8_t>&) -> std::shared_ptr<vfs::directory> {
    return SfcPakBuilder::makeSystemPak(
        EmbeddedFirmware::SfcIpl,    EmbeddedFirmware::SfcIplSize,
        EmbeddedFirmware::SfcBoards, EmbeddedFirmware::SfcBoardsSize);
}

auto cartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
    CartridgePak result;
    auto info = SfcPakBuilder::detectHeader(rom, romSize);
    if(info.board.empty()) {
        result.error = "could not detect ROM header";
        return result;
    }
    result.pak    = SfcPakBuilder::makeCartridgePak(info, rom, romSize);
    result.title  = info.title;
    result.region = info.region;
    return result;
}

auto slotPak(int, bool flash, const uint8_t* rom, size_t romSize)
    -> std::shared_ptr<vfs::directory> {
    return flash ? SfcPakBuilder::makeBsMemoryPak(rom, romSize)
                 : SfcPakBuilder::makeSufamiSlotPak(rom, romSize);
}

auto clearEntryPoints() -> void { ares::SuperFamicom::Thread::EntryPoints().clear(); }

const SystemDef kDef = {
    .id            = "sfc",
    .name          = "SNES / Super Famicom",
    .loadNameBase  = "[Nintendo] Super Famicom",
    .regions       = {"NTSC", "PAL"},
    .extensions    = {"sfc", "smc", "swc", "fig"},
    .systemNode    = "Super Famicom",
    .cartridgeNode = "Super Famicom Cartridge",
    .device        = "Gamepad",
    .ports         = 2,
    .buttons       = {
        {"B", 1u << 0}, {"Y", 1u << 1}, {"Select", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"A", 1u << 8}, {"X", 1u << 9}, {"L", 1u << 10}, {"R", 1u << 11},
    },
    .biosRequired  = false,
    .memBase       = 0x7E0000u,
    .memSize       = 0x20000u,
    .load          = loadSfc,
    .memRead       = memRead,
    .memWrite      = memWrite,
    .makeSystemPak = systemPak,
    .makeCartridgePak = cartridgePak,
    .makeSlotPak   = slotPak,
    .clearEntryPoints = clearEntryPoints,
};

const SystemRegistry::Registrar kRegistrar{&kDef};

} // namespace

// Link anchor for static builds — a self-registering TU is referenced by
// nothing, so archives would drop it; the iOS aggregator names this symbol
// to force the object (and its Registrar) into the image.
extern "C" int retro_emulator_core_sfc_link = 1;
