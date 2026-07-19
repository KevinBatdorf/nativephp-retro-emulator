// Vulkan display path for the Android host renderer.
//
// WHY Vulkan (not GLES): slang shaders run through librashader, whose runtimes
// are desktop-GL / Vulkan / Metal / D3D — there is NO GLES runtime, and our old
// renderer was GLES 2.0. On Android the only route to librashader is its Vulkan
// runtime, so this is a from-scratch Vulkan swapchain (ares has no Vulkan display
// backend to port). See .claude/findings.md → "Shaders".
//
// The frame path is deliberately pipeline-free for the passthrough (no-shader)
// case: the ares frame (BGRA in memory) is uploaded to a B8G8R8A8_UNORM source
// image and vkCmdBlitImage scales it (VK_FILTER_LINEAR) into the letterboxed
// output rect on the swapchain image — the blit does the scaling, centering, and
// format read, so no vertex/fragment SPIR-V is needed. With a shader active,
// librashader renders source -> an output image and that output is blitted
// instead. All Vulkan calls happen on the render thread (single-threaded use).
#pragma once

#include <cstdint>
#include <vector>

// Enables VkAndroidSurfaceCreateInfoKHR / vkCreateAndroidSurfaceKHR and the
// ANDROID_SURFACE structure-type enum (must precede <vulkan/vulkan.h>).
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>

struct ANativeWindow;

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    // Window-independent setup: instance, physical device, logical device,
    // graphics queue, command pool + per-frame command buffers/sync, the source
    // image + staging buffer. Called once. Returns false on any failure.
    bool initDevice();

    // Bind an Android surface and build the swapchain. Takes ownership of the
    // window (releases it in clearSurface). Safe to call again after clearSurface.
    bool setSurface(ANativeWindow* window);

    // Recreate the swapchain for a new size (surfaceChanged). No-op if unchanged.
    void onResize(int width, int height);

    // Tear down swapchain + surface + window (surfaceDestroyed). Device survives.
    void clearSurface();

    // True once a swapchain exists and frames can be presented.
    bool ready() const { return swapchain_ != VK_NULL_HANDLE; }

    // Copy one tightly-packed BGRA frame (w*h u32, no padding) into the mapped
    // staging buffer. Cheap (a memcpy) — call it under the frame mutex, then
    // release the lock before present(). Remembers w/h for the blit source rect.
    void stageFrame(const uint32_t* pixels, uint32_t w, uint32_t h);

    // Acquire, (upload + shade or passthrough), letterbox-blit into outRect on
    // the swapchain, and present. Returns false if there is no live swapchain.
    bool present(int outX, int outY, int outW, int outH);

    // Apply a librashader .slangp preset by path; nullptr/"" clears (passthrough).
    // Returns false only on a load/creation error (an EmulatorError-worthy event
    // upstream); clearing always succeeds.
    bool setShader(const char* path);

    // Read the last staged frame back as RGBA8 (w*h*4 bytes) for screenshots —
    // straight from the mapped staging buffer (BGRA->RGBA swizzle), no GPU op.
    bool screenshotRaw(std::vector<uint8_t>& rgbaOut, uint32_t& w, uint32_t& h);

    // Read the PRESENTED frame back as RGBA8: with an active filter chain this
    // is the post-shader output image (GPU readback on the render thread);
    // passthrough falls back to screenshotRaw — identical content either way.
    bool screenshotPresented(std::vector<uint8_t>& rgbaOut, uint32_t& w, uint32_t& h);

private:
    static constexpr int kFramesInFlight = 2;
    // Source image is fixed at the GL renderer's texture size (fits any SFC
    // canvas incl. overscan/interlace); frames upload into the top-left sub-rect.
    // Must cover the widest frame any core delivers — the PC Engine's
    // accurate VDP hands over up to 1365-wide time-based scanline buffers.
    static constexpr uint32_t kSrcW = 1536;
    static constexpr uint32_t kSrcH = 1024;
    static constexpr VkFormat kSrcFormat = VK_FORMAT_B8G8R8A8_UNORM;

    bool createSwapchain(uint32_t width, uint32_t height);
    void destroySwapchain();
    bool createSourceResources();
    bool ensureSourceImage(uint32_t w, uint32_t h);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    bool recordAndSubmit(uint32_t imageIndex, int outX, int outY, int outW, int outH);

    // librashader (created lazily on setShader; freed/replaced on change).
    void destroyShaderChain();
    bool ensureShaderOutput(uint32_t w, uint32_t h);
    bool ensureReadback(VkDeviceSize size);

    VkInstance       instance_       = VK_NULL_HANDLE;
    VkPhysicalDevice physical_       = VK_NULL_HANDLE;
    VkDevice         device_         = VK_NULL_HANDLE;
    uint32_t         graphicsFamily_ = 0;
    VkQueue          queue_          = VK_NULL_HANDLE;

    ANativeWindow*   window_         = nullptr;
    VkSurfaceKHR     surface_        = VK_NULL_HANDLE;
    VkSwapchainKHR   swapchain_      = VK_NULL_HANDLE;
    VkFormat         swapchainFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D       swapchainExtent_ = {0, 0};
    std::vector<VkImage> swapchainImages_;

    VkCommandPool    cmdPool_        = VK_NULL_HANDLE;
    VkCommandBuffer  cmdBuffers_[kFramesInFlight] = {};
    VkSemaphore      imageAvailable_[kFramesInFlight] = {};
    VkSemaphore      renderFinished_[kFramesInFlight] = {};
    VkFence          inFlight_[kFramesInFlight] = {};
    int              frame_          = 0;

    // Source (uploaded ares frame) + host-visible staging buffer.
    VkImage          srcImage_       = VK_NULL_HANDLE;
    VkDeviceMemory   srcMemory_      = VK_NULL_HANDLE;
    uint32_t         srcImgW_        = 0;   // current srcImage_ size (== frame size)
    uint32_t         srcImgH_        = 0;
    VkBuffer         staging_        = VK_NULL_HANDLE;
    VkDeviceMemory   stagingMemory_  = VK_NULL_HANDLE;
    void*            stagingMapped_  = nullptr;
    uint32_t         stagedW_        = 0;
    uint32_t         stagedH_        = 0;
    bool             hasFrame_       = false;

    // Shader-inclusive screenshot readback (lazily grown, persistently mapped).
    VkBuffer         readback_         = VK_NULL_HANDLE;
    VkDeviceMemory   readbackMemory_   = VK_NULL_HANDLE;
    void*            readbackMapped_   = nullptr;
    VkDeviceSize     readbackCapacity_ = 0;

    // librashader Vulkan filter chain (D). Null = passthrough.
    struct _filter_chain_vk* shaderChain_ = nullptr;
    uint64_t         frameCount_     = 0;
    // Shader output image (librashader renders into it; then blitted to swapchain).
    VkImage          shaderOut_      = VK_NULL_HANDLE;
    VkDeviceMemory   shaderOutMem_   = VK_NULL_HANDLE;
    uint32_t         shaderOutW_     = 0;
    uint32_t         shaderOutH_     = 0;
};
