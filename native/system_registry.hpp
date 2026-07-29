// Per-system dispatch for the compiled ares cores — shared by the Android JNI
// layer and the iOS C API. Keyed by system id: load function, pak node names,
// controller device, button bitmask, memory bus window.
#pragma once

#include <ares/ares.hpp>
#include <nall/vfs.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace SystemRegistry {

struct CartridgePak {
    std::shared_ptr<vfs::directory> pak;
    std::string title;
    std::string region;
    std::string error;   // non-empty on failure

    explicit operator bool() const { return (bool)pak; }
};

struct SystemDef {
    std::string id;             // ares system id, e.g. "sfc"
    std::string name;           // display name, e.g. "SNES / Super Famicom"

    // ares System::load() configuration name, composed per region exactly like
    // desktop (super-famicom.cpp:126: "[Nintendo] Super Famicom (" + region + ")").
    // regions lists the core's enumerate() variants in order; empty = the core
    // has no region variants (gb) and loadNameBase is passed through unchanged.
    std::string loadNameBase;
    std::vector<std::string> regions;

    // ROM file extensions accepted for this system (mia medium extensions()).
    // The bridge layers gate LoadRom on these before analysis, like desktop's
    // per-emulator file-dialog filters.
    std::vector<std::string> extensions;

    std::string systemNode;     // Platform::pak() node name for the system
    std::string cartridgeNode;  // Platform::pak() node name for the cartridge
    const char* device;         // controller device to allocate, nullptr = system-level controls (gb)
    int ports;                  // number of controller ports (0 = system-level controls)

    // Button name → bitmask bit. Bit positions are positional gamepad
    // semantics shared with the Kotlin/Swift input layers:
    //   0 = face south, 1 = face west, 8 = face east, 9 = face north,
    //   2 = select/mode, 3 = start, 4–7 = d-pad up/down/left/right,
    //   10 = L shoulder, 11 = R shoulder.
    std::unordered_map<std::string, uint32_t> buttons;

    // Memory bus window exposed to readMemory/writeMemory.
    uint32_t memBase;
    uint32_t memSize;

    bool (*load)(ares::Node::System& root, const SystemDef& def, const std::string& loadName);
    uint8_t (*memRead)(uint32_t offset);
    void (*memWrite)(uint32_t offset, uint8_t value);
    // bios carries an optional dev-supplied firmware image (LoadSystem biosPath).
    // gba uses it to override its embedded open BIOS; other systems ignore it.
    std::shared_ptr<vfs::directory> (*makeSystemPak)(const SystemDef& def,
                                                     const std::vector<uint8_t>& bios);
    CartridgePak (*makeCartridgePak)(const uint8_t* rom, size_t romSize);

    // Slotted-media pak builder (SuFami slots A/B, BS Memory); the platform
    // layers call through this so slot machinery lives with its core.
    // nullptr = the system has no slotted media.
    std::shared_ptr<vfs::directory> (*makeSlotPak)(int index, bool flash,
                                                   const uint8_t* rom, size_t romSize);

    // Purge this core's Thread::EntryPoints() static — Thread is a distinct
    // type per core namespace, so only the core can reach it (see
    // clearStaleEntryPoints).
    void (*clearEntryPoints)();

    // Boot options, by ares' own option() names ("Pixel Accuracy"). Applied
    // before load() — SNES picks its PPU implementation this way, and load()
    // crashes on a null implementation if it never runs. nullptr = the core
    // declares no options. getOption reads the live value back ("true"/"false",
    // empty for an unknown name); nullptr = nothing readable.
    bool (*setOption)(const std::string& name, const std::string& value);
    std::string (*getOption)(const std::string& name);
};

// Registers a compiled core at static-init time: dlopen (Android's modular
// build) or image load (iOS's static build) runs the constructor, so bundling
// a core IS registering it. all() orders by id, so registration order —
// which varies across TUs and load order — never leaks into behavior.
struct Registrar {
    explicit Registrar(const SystemDef* def);
};

// Compose the System::load() name for a region ("" or region-free system
// returns loadNameBase unchanged).
auto loadNameFor(const SystemDef& def, const std::string& region) -> std::string;

// Pick the boot region — a port of desktop's Emulator::region()
// (desktop-ui/emulator/emulator.cpp:40-60): walk the preferred list against
// the ROM's analyzed region list ("NTSC-J, NTSC-U" style CSV from the pak
// attribute); NTSC-U/NTSC-J preferences also match a plain "NTSC" entry;
// no preference hit falls back to the ROM's first listed region. Extensions
// beyond the reference: a non-empty regionOverride wins outright (dev knows
// best — junk homebrew headers), and an empty ROM list (desktop relies on
// mia always providing one) falls back to the first preference the system
// supports, then the system's first region. Region-free systems return "".
auto resolveRegion(const SystemDef& def,
                   const std::string& romRegionCsv,
                   const std::string& regionOverride,
                   const std::string& preferredCsv) -> std::string;

// Whether a ROM file extension (lowercase, no dot) is valid for this system.
auto extensionSupported(const SystemDef& def, const std::string& ext) -> bool;

// Ports/buttons JSON from registry data alone — GetPorts must answer after
// LoadSystem *stages* a system, before any core boots. Buttons are emitted in
// bitmask order, which matches the node-tree walk order on every compiled
// system.
auto staticPortsJson(const SystemDef& def) -> std::string;

// Compiled systems in display order. Stable pointers for the process lifetime.
auto all() -> const std::vector<const SystemDef*>&;

// Look up a compiled system by id; nullptr if this build does not include it.
auto find(const std::string& id) -> const SystemDef*;

// Clear every compiled core's pending Thread::EntryPoints() static. Works
// around an upstream ares bug: Thread::destroy() frees the coroutine but
// leaves its pending entry in the per-core-namespace EntryPoints() static —
// entries are only consumed when a coroutine first RUNS, so a system loaded
// but never powered (element with `system` but no `rom`, torn down early)
// strands {freed handle, dangling std::function} pairs. A later co_create
// that recycles the allocation matches the stale entry in Thread::Enter and
// runs a dead component's main() forever. One core runs per process, so
// after unload every pending entry is stale. Call from the platform destroy
// paths, after root->unload().
auto clearStaleEntryPoints() -> void;

} // namespace SystemRegistry
