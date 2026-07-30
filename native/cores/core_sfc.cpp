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
    // The platform applies staged boot options (setOption) before load. This
    // guard only covers callers that staged nothing: PPUBase dispatches
    // through an implementation pointer that stays NULL until setAccurate()
    // picks one, and load crashes in bus.reset() → ppu.map() on it.
    if (!ares::SuperFamicom::ppu.implementation) ares::SuperFamicom::ppu.setAccurate(false);
    return ares::SuperFamicom::load(root, nall::string(loadName.c_str()));
}

auto setOption(const std::string& name, const std::string& value) -> bool {
    return ares::SuperFamicom::option(nall::string(name.c_str()), nall::string(value.c_str()));
}

auto getOption(const std::string& name) -> std::string {
    if (name == "Pixel Accuracy") return ares::SuperFamicom::ppu.accurate ? "true" : "false";
    return {};
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
    .loadNameBase  = "[Nintendo] Super Famicom",
    .cartridgeNode = "Super Famicom Cartridge",
    .load          = loadSfc,
    .memRead       = memRead,
    .memWrite      = memWrite,
    .makeSystemPak = systemPak,
    .makeCartridgePak = cartridgePak,
    .makeSlotPak   = slotPak,
    .clearEntryPoints = clearEntryPoints,
    .setOption     = setOption,
    .getOption     = getOption,
};

const SystemRegistry::Registrar kRegistrar{&kDef};

} // namespace

// Link anchor for static builds — a self-registering TU is referenced by
// nothing, so archives would drop it; the iOS aggregator names this symbol
// to force the object (and its Registrar) into the image.
extern "C" int retro_emulator_core_sfc_link = 1;
