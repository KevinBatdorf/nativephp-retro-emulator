// Registry HOST — core-agnostic. The SystemDefs live with their cores
// (native/cores/core_<sys>.cpp) and self-register through Registrar at
// static-init time, so this file never links against any core: the modular
// Android build dlopens core libraries and the iOS static build just compiles
// the core files in — same registration path either way.
#include "system_registry.hpp"

#include <algorithm>

namespace SystemRegistry {

// Construct-on-first-use — Registrar constructors run during static init,
// before any deterministic ordering across translation units or dlopens.
static auto registry() -> std::vector<const SystemDef*>& {
    static std::vector<const SystemDef*> defs;
    return defs;
}

Registrar::Registrar(const SystemDef* def) {
    auto& defs = registry();
    defs.push_back(def);
    std::sort(defs.begin(), defs.end(),
              [](const SystemDef* a, const SystemDef* b) { return a->id < b->id; });
}

auto all() -> const std::vector<const SystemDef*>& {
    return registry();
}

auto loadNameFor(const SystemDef& def, const std::string& region) -> std::string {
    if (region.empty()) return def.loadNameBase;
    return def.loadNameBase + " (" + region + ")";
}

auto find(const std::string& id) -> const SystemDef* {
    for (auto* def : all()) {
        if (def->id == id) return def;
    }
    return nullptr;
}

auto clearStaleEntryPoints() -> void {
    // Thread is a distinct type (with a distinct EntryPoints() static) inside
    // each core's namespace, so each core purges its own via the def hook.
    for (auto* def : all()) {
        if (def->clearEntryPoints) def->clearEntryPoints();
    }
}

} // namespace SystemRegistry
