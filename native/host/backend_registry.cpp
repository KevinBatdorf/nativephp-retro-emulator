#include "backend_registry.hpp"

#include <algorithm>
#include <mutex>

namespace EmuHost {

namespace {

struct Entry {
    std::string name;
    BackendFactory make;
    std::unique_ptr<Backend> instance;
};

// Construct-on-first-use — registrar constructors run during static init /
// dlopen, before any deterministic cross-TU ordering exists.
auto registry() -> std::vector<Entry>& {
    static std::vector<Entry> entries;
    return entries;
}

// Guards instance creation: registry reads can arrive from any bridge thread
// (GetSystems runs from plain screens long before a surface exists).
// Registration itself happens inside static init / dlopen, which the
// platforms already serialize.
auto instanceMutex() -> std::mutex& {
    static std::mutex m;
    return m;
}

auto instanceOf(Entry& entry) -> Backend* {
    std::lock_guard<std::mutex> lock(instanceMutex());
    if (!entry.instance) entry.instance = entry.make();
    return entry.instance.get();
}

} // namespace

BackendRegistrar::BackendRegistrar(const char* name, BackendFactory make) {
    auto& entries = registry();
    entries.push_back({name, std::move(make), nullptr});
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.name < b.name; });
}

namespace Backends {

auto names() -> std::vector<std::string> {
    std::vector<std::string> out;
    for (auto& entry : registry()) out.push_back(entry.name);
    return out;
}

auto byName(const std::string& name) -> Backend* {
    for (auto& entry : registry()) {
        if (entry.name == name) return instanceOf(entry);
    }
    return nullptr;
}

auto availableSystems() -> std::vector<std::string> {
    std::vector<std::string> ids;
    for (auto& entry : registry()) {
        for (auto& id : instanceOf(entry)->systems()) {
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

auto forSystem(const std::string& systemId,
               const std::string& preferred) -> Backend* {
    auto claims = [&](Backend* backend) {
        if (!backend) return false;
        auto ids = backend->systems();
        return std::find(ids.begin(), ids.end(), systemId) != ids.end();
    };

    if (!preferred.empty()) {
        auto* backend = byName(preferred);
        if (backend) return claims(backend) ? backend : nullptr;
        // Not a registered engine name: offer it to each backend as a
        // dynamic core (the libretro loader adopts "snes9x" by probing its .so).
        for (auto& entry : registry()) {
            auto* candidate = instanceOf(entry);
            if (candidate->adoptDynamicCore(preferred, systemId)) return candidate;
        }
        return nullptr;
    }
    // No name means the built-in engine, never a substituted pick — every
    // other engine runs only because a config named it.
    auto* ares = byName("ares");
    return claims(ares) ? ares : nullptr;
}

} // namespace Backends
} // namespace EmuHost
