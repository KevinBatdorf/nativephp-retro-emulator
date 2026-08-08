// Per-system dispatch for the compiled ares cores — the ares backend's
// internal registry. Keyed by system id: load function, pak builders,
// memory accessors, boot options. Engine-neutral system metadata lives in
// native/host/system_catalog.hpp; `id` is the join key.
#pragma once

#include <ares/ares.hpp>
#include <nall/vfs.hpp>

#include <cstdint>
#include <memory>
#include <string>
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
    std::string id;             // ares system id, e.g. "sfc" (catalog join key)

    // ares System::load() configuration name, composed per region exactly like
    // desktop (super-famicom.cpp:126: "[Nintendo] Super Famicom (" + region + ")").
    // The host resolves the region ("" for region-free systems) and
    // loadNameFor composes; see loadNameFor.
    std::string loadNameBase;

    std::string cartridgeNode;  // Platform::pak() node name for the cartridge

    bool (*load)(ares::Node::System& root, const SystemDef& def, const std::string& loadName);
    // Offsets are relative to the catalog's memBase; bounds are enforced by
    // the host against memSize before these run.
    uint8_t (*memRead)(uint32_t offset);
    void (*memWrite)(uint32_t offset, uint8_t value);
    // bios carries an optional dev-supplied firmware image (LoadSystem biosPath).
    // gba uses it to override its embedded open BIOS; other systems ignore it.
    std::shared_ptr<vfs::directory> (*makeSystemPak)(const SystemDef& def,
                                                     const std::vector<uint8_t>& bios);
    CartridgePak (*makeCartridgePak)(const uint8_t* rom, size_t romSize);

    // Slotted-media pak builder (SuFami slots A/B, BS Memory); the backend
    // calls through this so slot machinery lives with its core.
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

    // True while the console's boot animation runs; nullptr = none detectable.
    bool (*inBootIntro)() = nullptr;
};

// Registers a compiled core at static-init time: dlopen (Android's modular
// build) or image load (iOS's static build) runs the constructor, so bundling
// a core IS registering it. all() orders by id, so registration order —
// which varies across TUs and load order — never leaks into behavior.
struct Registrar {
    explicit Registrar(const SystemDef* def);
};

// Compose the System::load() name for a host-resolved region ("" — the
// region-free case — passes loadNameBase through unchanged).
auto loadNameFor(const SystemDef& def, const std::string& region) -> std::string;

// Compiled systems ordered by id. Stable pointers for the process lifetime.
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
// after unload every pending entry is stale. Call from the backend teardown
// paths, after root->unload().
auto clearStaleEntryPoints() -> void;

} // namespace SystemRegistry
