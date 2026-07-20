// Nintendo 64 core module — self-registers via SystemRegistry::Registrar.
//
// N64 is the only core that renders through a GPU: ares drives it with the
// vendored Vulkan paraLLEl-RDP renderer (see the n64_core sources + VULKAN
// define in CMakeLists). Frames still arrive through the standard
// Node::Video::Screen path, so the display layer is unchanged. Firmware
// (pif.ntsc/pal.rom) is embedded; the CIC is attribute-driven (multi_pak).
#include "system_registry.hpp"

#include <n64/n64.hpp>

#include "pak/multi_pak.hpp"

namespace {

using SystemRegistry::CartridgePak;
using SystemRegistry::SystemDef;

auto loadN64(ares::Node::System& root, const SystemDef&, const std::string& loadName) -> bool {
    return ares::Nintendo64::load(root, nall::string(loadName.c_str()));
}

// Pre-load options, exactly the set desktop passes before Nintendo64::load
// (desktop-ui/emulator/nintendo-64.cpp:107-118). Defaults mirror desktop's
// settings.hpp:45-119 — critically Recompiler=true: the CPU/RSP recompilers
// ship DISABLED in the core (cpu.hpp:1204, rsp.hpp:683) and desktop switches
// them on here. Skipping this call runs the interpreters — ~4x slower.
// On iOS Accuracy::* force the interpreter at compile time and the Recompiler
// option compiles to a no-op; every other option applies the same as Android.
auto applyN64Options(const std::map<std::string, std::string>& options) -> void {
    const auto value = [&](const char* key, const char* fallback) -> std::string {
        auto it = options.find(key);
        return it == options.end() ? fallback : it->second;
    };

    ares::Nintendo64::option("Quality", value("quality", "SD").c_str());
    ares::Nintendo64::option("Supersampling", value("supersampling", "false").c_str());
    // Desktop's #if defined(VULKAN) branch — both our platforms compile it.
    ares::Nintendo64::option("Enable GPU acceleration", "true");
    ares::Nintendo64::option("Disable Video Interface Processing",
                             value("disableVideoInterfaceProcessing", "false").c_str());
    ares::Nintendo64::option("Weave Deinterlacing", value("weaveDeinterlacing", "true").c_str());
    ares::Nintendo64::option("Homebrew Mode", value("homebrewMode", "false").c_str());
    ares::Nintendo64::option("Recompiler", value("recompiler", "true").c_str());
    ares::Nintendo64::option("Expansion Pak", value("expansionPak", "true").c_str());
    ares::Nintendo64::option("Controller Pak Banks",
                             value("controllerPakBanks", "32KiB (Default)").c_str());
}

// RDRAM byte window for readMemory/writeMemory (rdram.ram is the writable RAM).
auto memRead(uint32_t o) -> uint8_t { return ares::Nintendo64::rdram.ram.data[o]; }
auto memWrite(uint32_t o, uint8_t v) -> void { ares::Nintendo64::rdram.ram.data[o] = v; }

auto systemPak(const SystemDef& def, const std::vector<uint8_t>&) -> std::shared_ptr<vfs::directory> {
    return MultiPak::makeSystemPak(def.id);
}

auto cartridgePak(const uint8_t* rom, size_t romSize) -> CartridgePak {
    auto built = MultiPak::makeCartridgePak("n64", rom, romSize);
    return {built.pak, built.title, built.region, built.error};
}

// No-op: unlike the other cores, N64's Nintendo64::Thread is a lightweight
// clock token (n64.hpp), not a libco cothread — it registers no entry points,
// so there is nothing to clear between loads.
auto clearEntryPoints() -> void {}

const SystemDef kDef = {
    .id            = "n64",
    .name          = "Nintendo 64",
    .loadNameBase  = "[Nintendo] Nintendo 64",
    .regions       = {"NTSC", "PAL", "MPAL"},
    .extensions    = {"n64", "v64", "z64"},
    .systemNode    = "Nintendo 64",
    .cartridgeNode = "Nintendo 64 Cartridge",
    .device        = "Gamepad",
    .ports         = 4,
    // The N64 controller: d-pad, A/B, the four C-buttons, L/R/Z shoulders,
    // Start. The analog stick is two axes (X-Axis/Y-Axis), handled separately.
    .buttons       = {
        {"Up", 1u << 0}, {"Down", 1u << 1}, {"Left", 1u << 2}, {"Right", 1u << 3},
        {"B", 1u << 4}, {"A", 1u << 5},
        {"C-Up", 1u << 6}, {"C-Down", 1u << 7}, {"C-Left", 1u << 8}, {"C-Right", 1u << 9},
        {"L", 1u << 10}, {"R", 1u << 11}, {"Z", 1u << 12}, {"Start", 1u << 13},
    },
    .memBase       = 0x00000000u,
    .memSize       = 0x00800000u,   // 8 MiB RDRAM (base + Expansion Pak)
    .load          = loadN64,
    .memRead       = memRead,
    .memWrite      = memWrite,
    .makeSystemPak = systemPak,
    .makeCartridgePak = cartridgePak,
    .makeSlotPak   = nullptr,
    .clearEntryPoints = clearEntryPoints,
    .applyOptions  = applyN64Options,
};

const SystemRegistry::Registrar kRegistrar{&kDef};

} // namespace

// Link anchor for static builds — see the note in core_md.cpp.
extern "C" int retro_emulator_core_n64_link = 1;
