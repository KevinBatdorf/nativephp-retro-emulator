// Atari 2600 core module — self-registers via SystemRegistry::Registrar.
#include "system_registry.hpp"

#include <a26/a26.hpp>

#include "pak/multi_pak.hpp"

namespace {

using SystemRegistry::CartridgePak;
using SystemRegistry::SystemDef;

auto loadA26(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    if (!ares::Atari2600::load(root, nall::string(loadName.c_str()))) return false;
    // Upstream omission (a26/cpu/debugger.cpp): load() never creates
    // tracer.interrupt, but power() raises resetPending and the first
    // CPU::main null-derefs it. Append the node exactly like fc/gb do
    // (fc/cpu/debugger.cpp:14) until upstream grows one.
    auto& debugger = ares::Atari2600::cpu.debugger;
    if (!debugger.tracer.interrupt && ares::Atari2600::cpu.node) {
        debugger.tracer.interrupt = ares::Atari2600::cpu.node
            ->append<ares::Node::Debugger::Tracer::Notification>("Interrupt", "CPU");
    }
    return true;
}

// 128 bytes of RIOT RAM at $80-$FF — the 2600's only work RAM.
auto memRead(uint32_t o) -> uint8_t { return (uint8_t)ares::Atari2600::riot.ram[o]; }
auto memWrite(uint32_t o, uint8_t v) -> void { ares::Atari2600::riot.ram[o] = v; }

auto systemPak(const SystemDef& def, const std::vector<uint8_t>&) -> std::shared_ptr<vfs::directory> {
    return MultiPak::makeSystemPak(def.id);
}

auto cartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
    auto built = MultiPak::makeCartridgePak("a26", rom, romSize);
    return {built.pak, built.title, built.region, built.error};
}

auto clearEntryPoints() -> void { ares::Atari2600::Thread::EntryPoints().clear(); }

const SystemDef kDef = {
    .id            = "a26",
    .name          = "Atari 2600",
    .loadNameBase  = "[Atari] Atari 2600",
    .regions       = {"NTSC", "PAL", "SECAM"},
    .extensions    = {"a26", "bin"},
    .systemNode    = "Atari 2600",
    .cartridgeNode = "Atari 2600 Cartridge",
    .device        = "Gamepad",
    .ports         = 2,
    // Desktop wiring (atari-2600.cpp): Fire is the south face button.
    .buttons       = {
        {"Fire", 1u << 0},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
    },
    // Console switches (a26/system/controls.cpp) — many games start on Reset
    // and configure on Select, so they're pressable, not bridge-only.
    // Bits mirror desktop's pad wiring (start/select/bumpers/north).
    .systemButtons = {
        {"Reset", 1u << 3}, {"Select", 1u << 2},
        {"Left Difficulty", 1u << 10}, {"Right Difficulty", 1u << 11},
        {"TV Type", 1u << 9},
    },
    .biosRequired  = false,
    .memBase       = 0x80u,
    .memSize       = 0x80u,
    .load          = loadA26,
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
extern "C" int retro_emulator_core_a26_link = 1;
