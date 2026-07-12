// Per-system dispatch for the compiled ares cores — shared by the Android JNI
// layer and the iOS C API. Everything the platform layers previously hardcoded
// for SFC (load function, pak node names, controller device, button bitmask,
// memory bus window) lives here, keyed by system id.
#pragma once

#include <ares/ares.hpp>
#include <nall/vfs.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

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
    std::string loadName;       // ares System::load() configuration name
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

    bool biosRequired;

    // Memory bus window exposed to readMemory/writeMemory.
    uint32_t memBase;
    uint32_t memSize;

    bool (*load)(ares::Node::System& root, const SystemDef& def);
    uint8_t (*memRead)(uint32_t offset);
    void (*memWrite)(uint32_t offset, uint8_t value);
    std::shared_ptr<vfs::directory> (*makeSystemPak)(const SystemDef& def);
    CartridgePak (*makeCartridgePak)(const uint8_t* rom, size_t romSize);
};

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
// paths, after root->unload(). Revisit if multiple cores ever share a
// process; upstream PR planned (real fix belongs in Thread::destroy()).
auto clearStaleEntryPoints() -> void;

} // namespace SystemRegistry
