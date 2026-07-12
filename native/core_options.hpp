#pragma once

#include <string>

#include <ares/ares.hpp>

// Per-core boolean settings applier, shared by the Android and iOS bridges.
//
// ares exposes some emulation toggles (Color Emulation, Deep Black Boost,
// Interframe Blending, …) as per-core `Node::Setting::Boolean` nodes that only
// exist on the systems that declare them. The wrapper carries them as
// system-specific config keys; this maps a key to its ares node name (keeping
// ares' strings in one place) and applies it exactly as desktop-ui does.
namespace CoreOptions {

// nullptr for an unknown key.
inline const char* nodeName(const std::string& key) {
    if (key == "colorEmulation")     return "Color Emulation";
    if (key == "deepBlackBoost")     return "Deep Black Boost";
    if (key == "interframeBlending") return "Interframe Blending";
    return nullptr;
}

// Verbatim port of desktop-ui Emulator::setBoolean (emulator.cpp:232-240):
// scan for the named node and set it. Returns false when the key is unknown or
// the loaded core doesn't declare the node — so callers can apply every toggle
// unconditionally and unsupported ones silently no-op. Must run on the thread
// that owns the core (the GL/emu thread).
inline bool applyBoolean(ares::Node::System root, const std::string& key, bool value) {
    if (!root) return false;
    const char* name = nodeName(key);
    if (!name) return false;
    if (auto node = root->scan<ares::Node::Setting::Boolean>(name)) {
        node->setValue(value);  // setValue() skips modify() when unchanged;
        node->modify(value);    // call modify() too or the first set never lands.
        return true;
    }
    return false;
}

// Current node value, or -1 when the key is unknown / the core lacks the node.
// Test seam: lets instrumented tests read back what Platform will see.
inline int readBoolean(ares::Node::System root, const std::string& key) {
    if (!root) return -1;
    const char* name = nodeName(key);
    if (!name) return -1;
    if (auto node = root->scan<ares::Node::Setting::Boolean>(name)) {
        return node->value() ? 1 : 0;
    }
    return -1;
}

}  // namespace CoreOptions
