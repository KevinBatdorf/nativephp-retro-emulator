// Engine-neutral facts about each cartridge system: identity, ROM
// extensions, controller topology, canonical button bits, connectable
// devices, and the work-RAM bus window. SNES has B/Y/Select/… regardless of
// which engine emulates it, so this data lives with the host, not any
// backend.
//
// Bit positions are the plugin's public positional-gamepad contract, shared
// with the Kotlin/Swift input layers and the PHP Buttons enums:
//   0 = face south, 1 = face west, 8 = face east, 9 = face north,
//   2 = select/mode, 3 = start, 4–7 = d-pad up/down/left/right,
//   10 = L shoulder, 11 = R shoulder.
// tests/PluginTest.php's drift scanner parses the .buttons blocks in
// system_catalog.cpp against those enums — keep the literal style.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace SystemCatalog {

// A connectable device's inputs: button name → positional bit, plus axis
// names (mouse X/Y).
struct DeviceDescriptor {
    std::unordered_map<std::string, uint32_t> buttons;
    std::vector<std::string> axes;
};

struct System {
    std::string id;             // shared system id, e.g. "sfc"
    std::string name;           // display name, e.g. "SNES / Super Famicom"

    // ROM file extensions accepted for this system (lowercase, no dot) — the
    // bridge layers gate LoadRom on these before analysis.
    std::vector<std::string> extensions;

    // Region variants the system boots as, in preference-fallback order;
    // empty = region-free (handhelds). Feeds the host's resolveRegion.
    std::vector<std::string> regions;

    const char* device;         // default controller device; nullptr = the
                                // controls live on the system itself (gb/gba)
    int ports;                  // physical controller ports (0 = built-in)

    // Canonical button name → positional bit for the default device.
    std::unordered_map<std::string, uint32_t> buttons;

    // Non-default connectable devices (e.g. the SNES Mouse), and the
    // multitap container if the system has one (name + logical fan-out).
    std::unordered_map<std::string, DeviceDescriptor> extraDevices;
    const char* multitapName;   // nullptr = no multitap
    int multitapBlock;

    // Work-RAM bus window exposed to readMemory/writeMemory.
    uint32_t memBase;
    uint32_t memSize;
};

// All cataloged systems, ordered by id (the order every "supported systems"
// list has always used). Stable pointers for the process lifetime.
auto all() -> const std::vector<const System*>&;

// Look up by id; nullptr when the catalog has no such system. NOTE: catalog
// presence is not availability — a system is loadable only when a registered
// backend claims it (BackendRegistry::availableSystems).
auto find(const std::string& id) -> const System*;

// Whether a ROM extension (lowercase, no dot) is valid for this system.
auto extensionSupported(const System& sys, const std::string& ext) -> bool;

// The device a physical port effectively carries: the explicit registration,
// or — port 1 only — the system's own default pad (the auto-connect rule).
// Every read path must use this or it disagrees with the live engine.
auto effectiveDeviceName(const System& sys, int physicalPort,
                         const std::string& registered) -> std::string;

// Resolve a device name to its descriptor (default pad or extras table).
// False when the system doesn't support the device.
auto resolveDevice(const System& sys, const std::string& name,
                   DeviceDescriptor& out) -> bool;

// Device names this system accepts: the default pad + any table extras.
auto supportedDevices(const System& sys) -> std::vector<std::string>;

// Multitap helpers: whether `name` is this system's multitap, and how many
// logical ports a device on a physical port consumes (multitap block, else 1).
auto isMultitap(const System& sys, const std::string& name) -> bool;
auto portBlock(const System& sys, const std::string& name) -> int;

} // namespace SystemCatalog
