#include "system_registry.hpp"

#include <sfc/sfc.hpp>
#include <fc/fc.hpp>
#include <gb/gb.hpp>
#include <md/md.hpp>

#include "pak/sfc_pak.hpp"
#include "pak/multi_pak.hpp"
#include "embedded_firmware.hpp"

namespace SystemRegistry {

// --- load thunks -------------------------------------------------------------

static auto loadSfc(ares::Node::System& root, const SystemDef& def) -> bool {
    // PPUBase dispatches through an implementation pointer that is NULL until
    // setAccurate() picks one — the desktop frontend always sets the
    // "Pixel Accuracy" option before load; without this, load crashes in
    // bus.reset() → ppu.map(). false = the performance PPU.
    ares::SuperFamicom::ppu.setAccurate(false);
    return ares::SuperFamicom::load(root, nall::string(def.loadName.c_str()));
}
static auto loadFc(ares::Node::System& root, const SystemDef& def) -> bool {
    return ares::Famicom::load(root, nall::string(def.loadName.c_str()));
}
static auto loadGb(ares::Node::System& root, const SystemDef& def) -> bool {
    return ares::GameBoy::load(root, nall::string(def.loadName.c_str()));
}
static auto loadMd(ares::Node::System& root, const SystemDef& def) -> bool {
    return ares::MegaDrive::load(root, nall::string(def.loadName.c_str()));
}

// --- memory windows ----------------------------------------------------------
// Offsets are relative to memBase; bounds are enforced by the caller against
// memSize before these are invoked.

static auto sfcMemRead(uint32_t o) -> uint8_t { return ares::SuperFamicom::cpu.wram[o]; }
static auto sfcMemWrite(uint32_t o, uint8_t v) -> void { ares::SuperFamicom::cpu.wram[o] = v; }

static auto fcMemRead(uint32_t o) -> uint8_t { return (uint8_t)ares::Famicom::cpu.ram[o]; }
static auto fcMemWrite(uint32_t o, uint8_t v) -> void { ares::Famicom::cpu.ram[o] = v; }

static auto gbMemRead(uint32_t o) -> uint8_t { return (uint8_t)ares::GameBoy::cpu.wram[o]; }
static auto gbMemWrite(uint32_t o, uint8_t v) -> void { ares::GameBoy::cpu.wram[o] = v; }

// 68000 work RAM is stored as 16-bit words; even bus byte = high byte.
static auto mdMemRead(uint32_t o) -> uint8_t {
    uint16_t word = (uint16_t)ares::MegaDrive::cpu.ram[o >> 1];
    return (o & 1) ? (uint8_t)(word & 0xff) : (uint8_t)(word >> 8);
}
static auto mdMemWrite(uint32_t o, uint8_t v) -> void {
    auto& word = ares::MegaDrive::cpu.ram[o >> 1];
    if(o & 1) word.byte(0) = v;
    else      word.byte(1) = v;
}

// --- pak thunks ---------------------------------------------------------------

static auto sfcSystemPak(const SystemDef&) -> std::shared_ptr<vfs::directory> {
    return SfcPakBuilder::makeSystemPak(
        EmbeddedFirmware::SfcIpl,    EmbeddedFirmware::SfcIplSize,
        EmbeddedFirmware::SfcBoards, EmbeddedFirmware::SfcBoardsSize);
}
static auto miaSystemPak(const SystemDef& def) -> std::shared_ptr<vfs::directory> {
    return MultiPak::makeSystemPak(def.id);
}

static auto sfcCartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
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

template<const char* SystemId>
static auto miaCartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
    auto built = MultiPak::makeCartridgePak(SystemId, rom, romSize);
    return {built.pak, built.title, built.region, built.error};
}

static constexpr char kFc[] = "fc";
static constexpr char kGb[] = "gb";
static constexpr char kMd[] = "md";

// --- the registry --------------------------------------------------------------

static const SystemDef kSfcDef = {
    .id            = "sfc",
    .name          = "SNES / Super Famicom",
    .loadName      = "[Nintendo] Super Famicom (NTSC)",
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
    .memRead       = sfcMemRead,
    .memWrite      = sfcMemWrite,
    .makeSystemPak = sfcSystemPak,
    .makeCartridgePak = sfcCartridgePak,
};

static const SystemDef kFcDef = {
    .id            = "fc",
    .name          = "NES / Famicom",
    .loadName      = "[Nintendo] Famicom (NTSC-U)",
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
    .memRead       = fcMemRead,
    .memWrite      = fcMemWrite,
    .makeSystemPak = miaSystemPak,
    .makeCartridgePak = miaCartridgePak<kFc>,
};

static const SystemDef kGbDef = {
    .id            = "gb",
    .name          = "Game Boy",
    .loadName      = "[Nintendo] Game Boy",
    .systemNode    = "Game Boy",
    .cartridgeNode = "Game Boy Cartridge",
    .device        = nullptr,   // controls live on the system node
    .ports         = 0,
    .buttons       = {
        {"B", 1u << 0}, {"Select", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"A", 1u << 8},
    },
    .biosRequired  = false,
    .memBase       = 0xC000u,
    .memSize       = 0x2000u,
    .load          = loadGb,
    .memRead       = gbMemRead,
    .memWrite      = gbMemWrite,
    .makeSystemPak = miaSystemPak,
    .makeCartridgePak = miaCartridgePak<kGb>,
};

static const SystemDef kMdDef = {
    .id            = "md",
    .name          = "Sega Mega Drive / Genesis",
    .loadName      = "[Sega] Mega Drive (NTSC-U)",
    .systemNode    = "Mega Drive",
    .cartridgeNode = "Mega Drive Cartridge",
    .device        = "Fighting Pad",
    .ports         = 2,
    // Follows the common libretro-style Genesis layout: A/B/C on
    // west/south/east, X/Y/Z on north/L/R shoulders.
    .buttons       = {
        {"B", 1u << 0}, {"A", 1u << 1}, {"Mode", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"C", 1u << 8}, {"X", 1u << 9}, {"Y", 1u << 10}, {"Z", 1u << 11},
    },
    .biosRequired  = false,
    .memBase       = 0xFF0000u,
    .memSize       = 0x10000u,
    .load          = loadMd,
    .memRead       = mdMemRead,
    .memWrite      = mdMemWrite,
    .makeSystemPak = miaSystemPak,
    .makeCartridgePak = miaCartridgePak<kMd>,
};

auto all() -> const std::vector<const SystemDef*>& {
    static const std::vector<const SystemDef*> systems = {&kFcDef, &kSfcDef, &kGbDef, &kMdDef};
    return systems;
}

auto find(const std::string& id) -> const SystemDef* {
    for(auto* def : all()) {
        if(def->id == id) return def;
    }
    return nullptr;
}

auto clearStaleEntryPoints() -> void {
    // Thread is a distinct type (with a distinct EntryPoints() static) inside
    // each core's namespace — scheduler/thread.hpp is included per system.
    ares::Famicom::Thread::EntryPoints().clear();
    ares::SuperFamicom::Thread::EntryPoints().clear();
    ares::GameBoy::Thread::EntryPoints().clear();
    ares::MegaDrive::Thread::EntryPoints().clear();
}

} // namespace SystemRegistry
