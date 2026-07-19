// PlayStation core module — self-registers via SystemRegistry::Registrar.
//
// The first disc system: media loads by PATH (mia/medium/playstation.cpp via
// MultiPak::makeMediaPak — the pak carries cd.rom through vfs::cdrom), the
// game connects through the "PlayStation/Disc Tray" port, and the 128 KiB
// Memory Card is its own pak + port (desktop-ui playstation.cpp:120-126).
//
// biosRequired: the PS1 BIOS is copyrighted commercial firmware that never
// ships with this plugin — devs supply their own dump via the LoadSystem
// config's biosPath (desktop-ui playstation.cpp:18-20 matches per-region
// images by hash; we trust the dev's file).
#include "system_registry.hpp"

#include <ps1/ps1.hpp>

#include "pak/multi_pak.hpp"

namespace {

using SystemRegistry::CartridgePak;
using SystemRegistry::SystemDef;

auto loadPs1(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    return ares::PlayStation::load(root, nall::string(loadName.c_str()));
}

// 2MB main RAM at KUSEG 0x00000000 (ps1/memory/bus.hpp:2).
auto memRead(uint32_t o) -> uint8_t { return ares::PlayStation::cpu.ram.data[o]; }
auto memWrite(uint32_t o, uint8_t v) -> void { ares::PlayStation::cpu.ram.data[o] = v; }

auto systemPak(const SystemDef& def, const std::vector<uint8_t>& bios) -> std::shared_ptr<vfs::directory> {
    return MultiPak::makeSystemPak(def.id, bios);
}

auto mediaPak(const char* path) -> CartridgePak {
    auto built = MultiPak::makeMediaPak("ps1", path);
    return {built.pak, built.title, built.region, built.error};
}

auto clearEntryPoints() -> void { ares::PlayStation::Thread::EntryPoints().clear(); }

const SystemDef kDef = {
    .id            = "ps1",
    .name          = "PlayStation",
    .loadNameBase  = "[Sony] PlayStation",
    .regions       = {"NTSC-J", "NTSC-U", "PAL"},   // system.cpp enumerate() order
    .extensions    = {"cue", "exe", "ps-exe"},
    .systemNode    = "PlayStation",
    .cartridgeNode = "PlayStation Disc",
    .device        = "Digital Gamepad",
    .ports         = 2,
    // desktop-ui playstation.cpp:25-40 — Digital Gamepad on the positional
    // gamepad bits (Cross=south, Circle=east, Square=west, Triangle=north).
    .buttons       = {
        {"Cross", 1u << 0}, {"Square", 1u << 1}, {"Select", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"Circle", 1u << 8}, {"Triangle", 1u << 9},
        {"L1", 1u << 10}, {"R1", 1u << 11}, {"L2", 1u << 12}, {"R2", 1u << 13},
    },
    .biosRequired  = true,
    .memBase       = 0x00000000u,
    .memSize       = 0x200000u,
    .load          = loadPs1,
    .memRead       = memRead,
    .memWrite      = memWrite,
    .makeSystemPak = systemPak,
    .makeCartridgePak = nullptr,   // disc system — media loads by path
    .makeSlotPak   = nullptr,
    .clearEntryPoints = clearEntryPoints,
    // Desktop addresses it by PATH ("PlayStation/Disc Tray"); the node's own
    // name — what our findByName walk matches — is just "Disc Tray"
    // (ps1/disc/disc.cpp:18).
    .mediaPort     = "Disc Tray",
    .makeMediaPak  = mediaPak,
    .extraPaks     = {
        {"Memory Card", "save.card", 128 * 1024, "Memory Card Port 1", "Memory Card"},
    },
};

const SystemRegistry::Registrar kRegistrar{&kDef};

} // namespace

// Link anchor for static builds — see the note in core_sfc.cpp.
extern "C" int retro_emulator_core_ps1_link = 1;
