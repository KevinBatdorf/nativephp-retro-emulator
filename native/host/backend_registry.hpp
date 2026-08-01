// Registry of emulation backends, mirroring SystemRegistry's pattern one
// level up: a backend library registers a factory at static-init time, so on
// Android dlopening libbackend_<name>.so IS registering it, and on iOS the
// static build registers through link anchors. The copy-assets hook decides
// what ships; this code only discovers what arrived.
//
// Backends are singletons here: engines assume one instance per process, and
// capability/system queries arrive long before (and independent of) any
// boot. The registry creates each backend once, on first use, and hands out
// stable pointers.
#pragma once

#include "backend.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace EmuHost {

using BackendFactory = std::function<std::unique_ptr<Backend>()>;

struct BackendRegistrar {
    BackendRegistrar(const char* name, BackendFactory make);
};

namespace Backends {

// Names of every registered backend, sorted.
auto names() -> std::vector<std::string>;

// The backend registered under `name`; nullptr if absent from this build.
auto byName(const std::string& name) -> Backend*;

// Union of every registered backend's systems(), sorted + deduped by id —
// what "supported systems" means once more than one engine exists.
auto availableSystems() -> std::vector<std::string>;

// Pick the engine that boots `systemId`. `preferred` (a backend name) wins
// when that backend claims the system — and is an explicit nullptr (not a
// fallback) when it doesn't, so a dev asking for a specific engine never
// silently gets another. "" runs the built-in engine (ares). nullptr when
// nothing claims the system.
auto forSystem(const std::string& systemId,
               const std::string& preferred = "") -> Backend*;

} // namespace Backends
} // namespace EmuHost
