// Mega Drive / Genesis core module — self-registers via SystemRegistry::Registrar.
#include "system_registry.hpp"

#include <md/md.hpp>

#include "pak/multi_pak.hpp"

namespace {

using SystemRegistry::CartridgePak;
using SystemRegistry::SystemDef;

auto loadMd(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    return ares::MegaDrive::load(root, nall::string(loadName.c_str()));
}

// 68000 work RAM is stored as 16-bit words; even bus byte = high byte.
auto memRead(uint32_t o) -> uint8_t {
    uint16_t word = (uint16_t)ares::MegaDrive::cpu.ram[o >> 1];
    return (o & 1) ? (uint8_t)(word & 0xff) : (uint8_t)(word >> 8);
}
auto memWrite(uint32_t o, uint8_t v) -> void {
    auto& word = ares::MegaDrive::cpu.ram[o >> 1];
    if(o & 1) word.byte(0) = v;
    else      word.byte(1) = v;
}

auto systemPak(const SystemDef& def, const std::vector<uint8_t>&) -> std::shared_ptr<vfs::directory> {
    return MultiPak::makeSystemPak(def.id);
}

auto cartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
    auto built = MultiPak::makeCartridgePak("md", rom, romSize);
    return {built.pak, built.title, built.region, built.error};
}

auto clearEntryPoints() -> void { ares::MegaDrive::Thread::EntryPoints().clear(); }

// md declares Recompiler (32X-only, compile-gated) and TMSS. No live readback:
// the recompiler flag lives on 32X hardware this build doesn't compile.
auto setOption(const std::string& name, const std::string& value) -> bool {
    return ares::MegaDrive::option(nall::string(name.c_str()), nall::string(value.c_str()));
}

const SystemDef kDef = {
    .id            = "md",
    .loadNameBase  = "[Sega] Mega Drive",
    .cartridgeNode = "Mega Drive Cartridge",
    .load          = loadMd,
    .memRead       = memRead,
    .memWrite      = memWrite,
    .makeSystemPak = systemPak,
    .makeCartridgePak = cartridgePak,
    .makeSlotPak   = nullptr,
    .clearEntryPoints = clearEntryPoints,
    .setOption     = setOption,
};

const SystemRegistry::Registrar kRegistrar{&kDef};

} // namespace

// Link anchor for static builds — a self-registering TU is referenced by
// nothing, so archives would drop it; the iOS aggregator names this symbol
// to force the object (and its Registrar) into the image.
extern "C" int retro_emulator_core_md_link = 1;
