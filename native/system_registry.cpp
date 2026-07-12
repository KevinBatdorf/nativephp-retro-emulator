#include "system_registry.hpp"

#include <algorithm>

#include <sfc/sfc.hpp>
#include <fc/fc.hpp>
#include <gb/gb.hpp>
#include <md/md.hpp>

#include "pak/sfc_pak.hpp"
#include "pak/multi_pak.hpp"
#include "embedded_firmware.hpp"

namespace SystemRegistry {

// --- load thunks -------------------------------------------------------------

static auto loadSfc(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    // PPUBase dispatches through an implementation pointer that is NULL until
    // setAccurate() picks one — the desktop frontend always sets the
    // "Pixel Accuracy" option before load; without this, load crashes in
    // bus.reset() → ppu.map(). false = the performance PPU.
    ares::SuperFamicom::ppu.setAccurate(false);
    return ares::SuperFamicom::load(root, nall::string(loadName.c_str()));
}
static auto loadFc(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    return ares::Famicom::load(root, nall::string(loadName.c_str()));
}
static auto loadGb(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    return ares::GameBoy::load(root, nall::string(loadName.c_str()));
}
static auto loadMd(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    return ares::MegaDrive::load(root, nall::string(loadName.c_str()));
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
    .memRead       = sfcMemRead,
    .memWrite      = sfcMemWrite,
    .makeSystemPak = sfcSystemPak,
    .makeCartridgePak = sfcCartridgePak,
};

static const SystemDef kFcDef = {
    .id            = "fc",
    .name          = "NES / Famicom",
    .loadNameBase  = "[Nintendo] Famicom",
    .regions       = {"NTSC-J", "NTSC-U", "PAL"},
    .extensions    = {"fc", "nes", "unf", "unif", "unh"},
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
    .loadNameBase  = "[Nintendo] Game Boy",
    .regions       = {},   // region-free handheld
    .extensions    = {"gb", "gbc"},
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
    .loadNameBase  = "[Sega] Mega Drive",
    .regions       = {"NTSC-J", "NTSC-U", "PAL"},
    .extensions    = {"md", "gen", "bin"},
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

// --- region resolution ---------------------------------------------------------

// nall::split_and_strip equivalent for the "NTSC-J, NTSC-U" CSV form used by
// the pak "region" attribute and desktop's settings.boot.prefer.
static auto splitAndStrip(const std::string& csv) -> std::vector<std::string> {
    std::vector<std::string> out;
    size_t start = 0;
    while(start <= csv.size()) {
        size_t comma = csv.find(',', start);
        if(comma == std::string::npos) comma = csv.size();
        size_t a = start, b = comma;
        while(a < b && (csv[a] == ' ' || csv[a] == '\t')) a++;
        while(b > a && (csv[b - 1] == ' ' || csv[b - 1] == '\t')) b--;
        if(b > a) out.push_back(csv.substr(a, b - a));
        if(comma == csv.size()) break;
        start = comma + 1;
    }
    return out;
}

static auto contains(const std::vector<std::string>& list, const std::string& value) -> bool {
    for(auto& entry : list) if(entry == value) return true;
    return false;
}

auto loadNameFor(const SystemDef& def, const std::string& region) -> std::string {
    if(def.regions.empty() || region.empty()) return def.loadNameBase;
    return def.loadNameBase + " (" + region + ")";
}

auto resolveRegion(const SystemDef& def,
                   const std::string& romRegionCsv,
                   const std::string& regionOverride,
                   const std::string& preferredCsv) -> std::string {
    if(def.regions.empty()) return {};
    if(!regionOverride.empty()) return regionOverride;

    auto preferredRegions = splitAndStrip(preferredCsv.empty() ? "NTSC-U" : preferredCsv);
    auto regions = splitAndStrip(romRegionCsv);

    // Verbatim logic of desktop's Emulator::region() (emulator.cpp:41-55).
    if(!regions.empty()) {
        for(auto& prefer : preferredRegions) {
            if(contains(regions, prefer)) return prefer;
            if(prefer == "NTSC-U" || prefer == "NTSC-J") {
                if(contains(regions, "NTSC")) return "NTSC";
            }
        }
        // No preferred region found — first region in the ROM's list.
        // (Desktop note: required for unusual regions like NTSC-DEV on 64DD.)
        return regions.front();
    }

    // Beyond the reference: mia always provides a region so desktop never gets
    // here with a loaded game. An analyzer that yields nothing falls back to
    // the first preference the system supports, then the system's first region.
    for(auto& prefer : preferredRegions) {
        if(contains(def.regions, prefer)) return prefer;
    }
    return def.regions.front();
}

auto extensionSupported(const SystemDef& def, const std::string& ext) -> bool {
    return contains(def.extensions, ext);
}

auto staticPortsJson(const SystemDef& def) -> std::string {
    // Emit buttons in bitmask order — matches the node-tree walk order the
    // booted core reports on every compiled system.
    std::vector<std::pair<uint32_t, std::string>> ordered;
    ordered.reserve(def.buttons.size());
    for(auto& [name, bit] : def.buttons) ordered.push_back({bit, name});
    std::sort(ordered.begin(), ordered.end());

    auto buttonsJson = [&]() {
        std::string json = "[";
        for(auto& [bit, name] : ordered) {
            if(json.size() > 1) json += ",";
            json += "\"" + name + "\"";
        }
        return json + "]";
    }();

    std::string json = "[";
    int portCount = def.ports == 0 ? 1 : def.ports;
    for(int i = 0; i < portCount && i < 2; i++) {
        if(json.size() > 1) json += ",";
        json += "{\"port\":" + std::to_string(i + 1) +
                ",\"buttons\":" + buttonsJson + "}";
    }
    return json + "]";
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
