// Cross-module-safe ares node lookups. The modular Android build creates the
// node tree inside dlopen'd core libraries, and dynamic_cast across modules
// compares typeinfo by pointer there — Object::cast<T>()/find<T>() silently
// return empty from the frontend (observed: "no Cartridge Slot port found",
// no audio attach). identity() is an ordinary virtual returning the
// DeclareClass string — ABI-stable across modules — so these discriminate on
// it and static-cast, the same check Object::scan() itself uses. Exact-class
// matching only, which every platform-layer lookup satisfies.
#pragma once

#include <ares/ares.hpp>

#include <string>
#include <vector>

namespace NodeUtil {

namespace detail {
// _nodes is protected; a derived class may form a pointer-to-member to it and
// apply that to ANY Object — the standard-blessed way to walk children
// without patching ares.
struct Access : ares::Core::Object {
    static auto nodes(const ares::Node::Object& o)
        -> const std::vector<ares::Node::Object>& {
        constexpr auto member = &Access::_nodes;
        return (*o).*member;
    }
};
} // namespace detail

template<typename T>
inline auto as(const ares::Node::Object& node) -> T {
    using Type = typename T::element_type;
    if(node && node->identity() == Type::identifier()) {
        return std::static_pointer_cast<Type>(node);
    }
    return {};
}

template<typename T>
inline auto collect(const ares::Node::Object& node, std::vector<T>& out) -> void {
    if(auto typed = as<T>(node)) out.push_back(typed);
    for(auto& child : detail::Access::nodes(node)) collect<T>(child, out);
}

// Mirrors Object::find<T>(): the root itself counts, then the subtree.
template<typename T>
inline auto findAll(const ares::Node::Object& root) -> std::vector<T> {
    std::vector<T> out;
    if(root) collect<T>(root, out);
    return out;
}

// Mirrors Object::find<T>(name): first node of the class with that name.
template<typename T>
inline auto findByName(const ares::Node::Object& root, const std::string& name) -> T {
    if(!root) return {};
    if(auto typed = as<T>(root); typed && root->name() == name.c_str()) return typed;
    for(auto& child : detail::Access::nodes(root)) {
        if(auto found = findByName<T>(child, name)) return found;
    }
    return {};
}

} // namespace NodeUtil
