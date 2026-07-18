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
    // each core's namespace, so each core purges its own via the def hook.
    for(auto* def : all()) {
        if(def->clearEntryPoints) def->clearEntryPoints();
    }
}

} // namespace SystemRegistry
