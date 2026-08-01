// The .id/.buttons literal style is load-bearing: tests/PluginTest.php
// parses these blocks against the PHP Buttons enums.
#include "system_catalog.hpp"

#include <algorithm>

namespace SystemCatalog {

namespace {

const System kFc = {
    .id         = "fc",
    .name       = "NES / Famicom",
    .extensions = {"fc", "nes", "unf", "unif", "unh"},
    .regions    = {"NTSC-J", "NTSC-U", "PAL"},
    .device     = "Gamepad",
    .ports      = 2,
    .buttons    = {
        {"B", 1u << 0}, {"Select", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"A", 1u << 8},
    },
    .extraDevices = {},
    .multitapName = nullptr,
    .multitapBlock = 0,
    .memBase    = 0x0000u,
    .memSize    = 0x800u,
};

const System kGb = {
    .id         = "gb",
    .name       = "Game Boy",
    .extensions = {"gb"},
    .regions    = {},   // region-free handheld
    .device     = nullptr,   // controls live on the system node
    .ports      = 0,
    .buttons    = {
        {"B", 1u << 0}, {"Select", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"A", 1u << 8},
    },
    .extraDevices = {},
    .multitapName = nullptr,
    .multitapBlock = 0,
    .memBase    = 0xC000u,
    .memSize    = 0x2000u,
};

const System kGba = {
    .id         = "gba",
    .name       = "Game Boy Advance",
    .extensions = {"gba"},
    .regions    = {},   // region-free handheld
    .device     = nullptr,   // controls live on the system node
    .ports      = 0,
    .buttons    = {
        {"B", 1u << 0}, {"Select", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"A", 1u << 8}, {"L", 1u << 10}, {"R", 1u << 11},
    },
    .extraDevices = {},
    .multitapName = nullptr,
    .multitapBlock = 0,
    .memBase    = 0x02000000u,
    .memSize    = 0x40000u,
};

const System kGbc = {
    .id         = "gbc",
    .name       = "Game Boy Color",
    .extensions = {"gbc"},
    .regions    = {},
    .device     = nullptr,
    .ports      = 0,
    .buttons    = {
        {"B", 1u << 0}, {"Select", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"A", 1u << 8},
    },
    .extraDevices = {},
    .multitapName = nullptr,
    .multitapBlock = 0,
    .memBase    = 0xC000u,
    .memSize    = 0x2000u,
};

const System kMd = {
    .id         = "md",
    .name       = "Sega Mega Drive / Genesis",
    .extensions = {"md", "gen", "bin"},
    .regions    = {"NTSC-J", "NTSC-U", "PAL"},
    .device     = "Fighting Pad",
    .ports      = 2,
    // Follows the common libretro-style Genesis layout: A/B/C on
    // west/south/east, X/Y/Z on north/L/R shoulders.
    .buttons    = {
        {"B", 1u << 0}, {"A", 1u << 1}, {"Mode", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"C", 1u << 8}, {"X", 1u << 9}, {"Y", 1u << 10}, {"Z", 1u << 11},
    },
    .extraDevices = {},
    .multitapName = nullptr,
    .multitapBlock = 0,
    .memBase    = 0xFF0000u,
    .memSize    = 0x10000u,
};

const System kSfc = {
    .id         = "sfc",
    .name       = "SNES / Super Famicom",
    .extensions = {"sfc", "smc", "swc", "fig"},
    .regions    = {"NTSC", "PAL"},
    .device     = "Gamepad",
    .ports      = 2,
    .buttons    = {
        {"B", 1u << 0}, {"Y", 1u << 1}, {"Select", 1u << 2}, {"Start", 1u << 3},
        {"Up", 1u << 4}, {"Down", 1u << 5}, {"Left", 1u << 6}, {"Right", 1u << 7},
        {"A", 1u << 8}, {"X", 1u << 9}, {"L", 1u << 10}, {"R", 1u << 11},
    },
    // Super Multitap is a container: no inputs of its own; the host fans it
    // out to gamepad sub-ports.
    .extraDevices = {
        {"Mouse", {{{"Left", 1u << 0}, {"Right", 1u << 1}}, {"X", "Y"}}},
        {"Super Multitap", {{}, {}}},
    },
    .multitapName = "Super Multitap",
    .multitapBlock = 4,
    .memBase    = 0x7E0000u,
    .memSize    = 0x20000u,
};

} // namespace

auto all() -> const std::vector<const System*>& {
    static const std::vector<const System*> kAll = {
        &kFc, &kGb, &kGba, &kGbc, &kMd, &kSfc,
    };
    return kAll;
}

auto find(const std::string& id) -> const System* {
    for (auto* sys : all()) {
        if (sys->id == id) return sys;
    }
    return nullptr;
}

auto extensionSupported(const System& sys, const std::string& ext) -> bool {
    return std::find(sys.extensions.begin(), sys.extensions.end(), ext)
        != sys.extensions.end();
}

auto effectiveDeviceName(const System& sys, int physicalPort,
                         const std::string& registered) -> std::string {
    if (registered.empty() && physicalPort == 1 && sys.device) return sys.device;
    return registered;
}

auto resolveDevice(const System& sys, const std::string& name,
                   DeviceDescriptor& out) -> bool {
    if (sys.device && name == sys.device) {
        out.buttons = sys.buttons;
        out.axes.clear();   // no shipped default pad has analog axes
        return true;
    }
    auto it = sys.extraDevices.find(name);
    if (it != sys.extraDevices.end()) { out = it->second; return true; }
    return false;
}

auto supportedDevices(const System& sys) -> std::vector<std::string> {
    std::vector<std::string> v;
    if (sys.device) v.emplace_back(sys.device);
    for (auto& [name, desc] : sys.extraDevices) v.push_back(name);
    return v;
}

auto isMultitap(const System& sys, const std::string& name) -> bool {
    return sys.multitapName && name == sys.multitapName;
}

auto portBlock(const System& sys, const std::string& name) -> int {
    return isMultitap(sys, name) ? sys.multitapBlock : 1;
}

} // namespace SystemCatalog
