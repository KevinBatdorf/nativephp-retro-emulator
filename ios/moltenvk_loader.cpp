// iOS Vulkan bootstrap for the N64 core — MoltenVK statically linked.
//
// MoltenVK is merged into RetroEmulator.framework as a partially-linked object
// whose ONLY exported symbol is the ICD entrypoint vk_icdGetInstanceProcAddr
// (scripts/build_xcframework.sh localizes the rest). Every standard vk*
// function stays internal, so volk's identically-named global function-pointer
// VARIABLES (volk.c defines one per vk* entrypoint) never collide with
// MoltenVK's function definitions at link time.
//
// Granite's Context::init_loader(addr) seeds volk from the given PFN and sets
// its loader_init_once latch, so ares' own later init_loader(nullptr)
// (n64/vulkan/vulkan.cpp) early-returns success without ever dlopen()ing —
// no dylib to embed, nothing for a consuming app to wire up.
//
// Compiled as part of n64_core: context.hpp needs the paraLLEl-RDP include
// dirs and the volk-based VK_NO_PROTOTYPES header set.
#include "context.hpp"

// The ICD entrypoint MoltenVK exports; identical signature/behavior to
// vkGetInstanceProcAddr (declared manually — the vulkan headers only
// prototype the standard name, which volk owns as a variable here).
extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);

extern "C" bool ios_n64_init_vulkan_loader() {
    return ::Vulkan::Context::init_loader(
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(vk_icdGetInstanceProcAddr));
}
