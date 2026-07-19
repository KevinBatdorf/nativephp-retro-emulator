#include "vulkan_renderer.hpp"

#include <android/log.h>
#include <android/native_window.h>
#include <algorithm>
#include <cstring>

// librashader Vulkan runtime — direct-linked (liblibrashader.so in jniLibs).
// LIBRA_RUNTIME_VULKAN unlocks the libra_vk_* decls; vulkan.h is already included
// via the header above. Pinned to librashader-v0.5.1 (matches the vendored ABI 2
// header). See .claude/findings.md → "Shaders".
#define LIBRA_RUNTIME_VULKAN
#include <librashader/librashader.h>

#define LOG_TAG "AresVk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Check a VkResult, log + return false on failure. `what` names the call site.
#define VK_CHECK(expr, what)                                                   \
    do {                                                                       \
        VkResult _r = (expr);                                                  \
        if (_r != VK_SUCCESS) {                                                \
            LOGE("%s failed: VkResult=%d", (what), (int)_r);                   \
            return false;                                                      \
        }                                                                      \
    } while (0)

// Log a librashader error's message (via libra_error_write) and free the error.
static void logLibraError(const char* what, libra_error_t err) {
    char* msg = nullptr;
    if (libra_error_write(err, &msg) == 0 && msg) {
        LOGE("%s: %s", what, msg);
        libra_error_free_string(&msg);
    } else {
        LOGE("%s (no message available)", what);
    }
    libra_error_free(&err);
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Record an image-layout transition covering the single color mip/layer.
static void imageBarrier(VkCommandBuffer cmd, VkImage image,
                         VkImageLayout oldLayout, VkImageLayout newLayout,
                         VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                         VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

uint32_t VulkanRenderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    LOGE("no memory type for bits=%u props=%u", typeBits, props);
    return UINT32_MAX;
}

// ---------------------------------------------------------------------------
// Device / instance setup (window-independent)
// ---------------------------------------------------------------------------

bool VulkanRenderer::initDevice() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "retro-emulator";
    app.apiVersion = VK_API_VERSION_1_1;

    const char* instExts[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instExts;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance");

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) { LOGE("no Vulkan physical devices"); return false; }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    // Pick the first device exposing a graphics queue family. (Android devices
    // present a single GPU; present support is validated against the surface in
    // createSwapchain.)
    bool found = false;
    for (auto dev : devices) {
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, qprops.data());
        for (uint32_t i = 0; i < qcount; i++) {
            if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                physical_ = dev;
                graphicsFamily_ = i;
                found = true;
                break;
            }
        }
        if (found) break;
    }
    if (!found) { LOGE("no graphics queue family"); return false; }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_, &props);
    LOGI("Vulkan device: %s (API %u.%u.%u)", props.deviceName,
         VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
         VK_VERSION_PATCH(props.apiVersion));

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = graphicsFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* devExts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;
    VK_CHECK(vkCreateDevice(physical_, &dci, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, graphicsFamily_, 0, &queue_);

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = graphicsFamily_;
    VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &cmdPool_), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmdPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = kFramesInFlight;
    VK_CHECK(vkAllocateCommandBuffers(device_, &cbai, cmdBuffers_), "vkAllocateCommandBuffers");

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // first wait returns immediately
    for (int i = 0; i < kFramesInFlight; i++) {
        VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &imageAvailable_[i]), "vkCreateSemaphore(acquire)");
        VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &renderFinished_[i]), "vkCreateSemaphore(render)");
        VK_CHECK(vkCreateFence(device_, &fci, nullptr, &inFlight_[i]), "vkCreateFence");
    }

    if (!createSourceResources()) return false;

    // Prove librashader is linked + loadable and the ABI matches our header.
    LOGI("librashader ABI=%zu (header expects %d), API=%zu",
         (size_t)libra_instance_abi_version(), LIBRASHADER_CURRENT_ABI,
         (size_t)libra_instance_api_version());
    return true;
}

bool VulkanRenderer::createSourceResources() {
    // The source image is created lazily at the EXACT frame size by
    // ensureSourceImage(): librashader samples the whole input image as its
    // source (no sub-region), so a fixed oversized texture would render the game
    // at a fraction of the output. Here we set up only the persistently-mapped
    // staging buffer, sized for the largest frame (kSrcW×kSrcH).
    VkMemoryRequirements mreq{};
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    uint32_t type = 0;

    // Host-visible staging buffer, persistently mapped (kSrcW*kSrcH*4 bytes).
    const VkDeviceSize stagingSize = (VkDeviceSize)kSrcW * kSrcH * 4;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = stagingSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &staging_), "vkCreateBuffer(staging)");
    vkGetBufferMemoryRequirements(device_, staging_, &mreq);
    type = findMemoryType(mreq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return false;
    mai.allocationSize = mreq.size;
    mai.memoryTypeIndex = type;
    VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &stagingMemory_), "vkAllocateMemory(staging)");
    VK_CHECK(vkBindBufferMemory(device_, staging_, stagingMemory_, 0), "vkBindBufferMemory(staging)");
    VK_CHECK(vkMapMemory(device_, stagingMemory_, 0, stagingSize, 0, &stagingMapped_), "vkMapMemory(staging)");
    return true;
}

// (Re)create the source image at exactly w×h. librashader treats the whole input
// image as the source, so it must BE the frame — not a corner of a larger texture.
bool VulkanRenderer::ensureSourceImage(uint32_t w, uint32_t h) {
    if (srcImage_ && srcImgW_ == w && srcImgH_ == h) return true;
    if (device_) vkDeviceWaitIdle(device_);
    if (srcImage_)  { vkDestroyImage(device_, srcImage_, nullptr);  srcImage_ = VK_NULL_HANDLE; }
    if (srcMemory_) { vkFreeMemory(device_, srcMemory_, nullptr);   srcMemory_ = VK_NULL_HANDLE; }
    srcImgW_ = srcImgH_ = 0;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kSrcFormat;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &srcImage_) != VK_SUCCESS) { srcImage_ = VK_NULL_HANDLE; return false; }

    VkMemoryRequirements mreq{};
    vkGetImageMemoryRequirements(device_, srcImage_, &mreq);
    uint32_t type = findMemoryType(mreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mreq.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &mai, nullptr, &srcMemory_) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, srcImage_, srcMemory_, 0);
    srcImgW_ = w;
    srcImgH_ = h;
    return true;
}

// ---------------------------------------------------------------------------
// Surface / swapchain
// ---------------------------------------------------------------------------

bool VulkanRenderer::setSurface(ANativeWindow* window) {
    window_ = window;
    VkAndroidSurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    sci.window = window;
    VK_CHECK(vkCreateAndroidSurfaceKHR(instance_, &sci, nullptr, &surface_), "vkCreateAndroidSurfaceKHR");

    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical_, graphicsFamily_, surface_, &present);
    if (!present) { LOGE("graphics family cannot present to this surface"); return false; }

    return createSwapchain((uint32_t)ANativeWindow_getWidth(window),
                           (uint32_t)ANativeWindow_getHeight(window));
}

bool VulkanRenderer::createSwapchain(uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps),
             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {  // surface size undefined — use requested
        extent.width  = std::max(caps.minImageExtent.width,  std::min(caps.maxImageExtent.width,  width));
        extent.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, height));
    }
    if (extent.width == 0 || extent.height == 0) {
        LOGE("zero swapchain extent (%ux%u) — surface not ready", extent.width, extent.height);
        return false;
    }

    // We blit into the swapchain image, so it MUST support TRANSFER_DST.
    const VkImageUsageFlags wantUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
        LOGE("swapchain does not support TRANSFER_DST usage (flags=%u)", caps.supportedUsageFlags);
        return false;
    }

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fmtCount, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (auto& f : formats) {
        // UNORM (not SRGB): ares emits already-gamma'd pixels; an SRGB swapchain
        // would double-apply gamma on the blit.
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
    }
    swapchainFormat_ = chosen.format;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    // Handheld panels (e.g. the AYN Thor) are portrait-native, so currentTransform
    // is a 90°/270° rotation. Setting preTransform=currentTransform promises the
    // engine PRE-rotated content — which our straight blit does not produce, so
    // the frame shows up rotated. Prefer IDENTITY (the display controller rotates
    // for us) when the surface supports it; only fall back to currentTransform.
    VkSurfaceTransformFlagBitsKHR preTransform =
        (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
            : caps.currentTransform;
    LOGI("surface transform: current=0x%x supported=0x%x chosen=0x%x",
         caps.currentTransform, caps.supportedTransforms, preTransform);

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface_;
    sci.minImageCount = imageCount;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = wantUsage;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = preTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // vsync, always supported
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_), "vkCreateSwapchainKHR");

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    swapchainImages_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapchainImages_.data());
    swapchainExtent_ = extent;
    LOGI("swapchain created: %ux%u, %u images, format=%d", extent.width, extent.height, n, swapchainFormat_);
    return true;
}

void VulkanRenderer::destroySwapchain() {
    if (device_) vkDeviceWaitIdle(device_);
    if (swapchain_) { vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
    swapchainImages_.clear();
    swapchainExtent_ = {0, 0};
}

void VulkanRenderer::onResize(int width, int height) {
    if (!surface_) return;
    if ((uint32_t)width == swapchainExtent_.width && (uint32_t)height == swapchainExtent_.height) return;
    destroySwapchain();
    createSwapchain((uint32_t)width, (uint32_t)height);
}

void VulkanRenderer::clearSurface() {
    destroyShaderChain();
    destroySwapchain();
    if (surface_) { vkDestroySurfaceKHR(instance_, surface_, nullptr); surface_ = VK_NULL_HANDLE; }
    if (window_) { ANativeWindow_release(window_); window_ = nullptr; }
}

// ---------------------------------------------------------------------------
// Frame upload + present
// ---------------------------------------------------------------------------

void VulkanRenderer::stageFrame(const uint32_t* pixels, uint32_t w, uint32_t h) {
    if (!stagingMapped_ || !pixels) return;
    if (w > kSrcW || h > kSrcH) {
        // Never clamp-and-copy: rows would land at the wrong stride and the
        // whole picture shears diagonally (hit by the PC Engine's 1128+ wide
        // accurate-VDP frames when the ceiling was 1024). Drop the frame loudly.
        LOGE("stageFrame: %ux%u exceeds staging %ux%u — frame dropped", w, h, kSrcW, kSrcH);
        return;
    }
    std::memcpy(stagingMapped_, pixels, (size_t)w * h * 4);
    stagedW_ = w;
    stagedH_ = h;
    hasFrame_ = true;
}

bool VulkanRenderer::present(int outX, int outY, int outW, int outH) {
    if (!swapchain_) return false;

    vkWaitForFences(device_, 1, &inFlight_[frame_], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                         imageAvailable_[frame_], VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        onResize((int)ANativeWindow_getWidth(window_), (int)ANativeWindow_getHeight(window_));
        return true;  // skip this frame; next one uses the fresh swapchain
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        LOGE("vkAcquireNextImageKHR failed: %d", (int)acq);
        return false;
    }

    vkResetFences(device_, 1, &inFlight_[frame_]);

    if (!recordAndSubmit(imageIndex, outX, outY, outW, outH)) return false;

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderFinished_[frame_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    VkResult pres = vkQueuePresentKHR(queue_, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
        onResize((int)ANativeWindow_getWidth(window_), (int)ANativeWindow_getHeight(window_));
    } else if (pres != VK_SUCCESS) {
        LOGE("vkQueuePresentKHR failed: %d", (int)pres);
    }

    frame_ = (frame_ + 1) % kFramesInFlight;
    return true;
}

bool VulkanRenderer::recordAndSubmit(uint32_t imageIndex, int outX, int outY, int outW, int outH) {
    VkCommandBuffer cmd = cmdBuffers_[frame_];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");

    VkImage swap = swapchainImages_[imageIndex];
    // Only blit a real frame into a non-empty output rect; otherwise just clear
    // (e.g. the no-ROM present(0,0,0,0) after stopEmulation).
    bool draw = hasFrame_ && stagedW_ > 0 && stagedH_ > 0 && outW > 0 && outH > 0;
    // The source image must match the frame size exactly (see ensureSourceImage).
    if (draw && !ensureSourceImage(stagedW_, stagedH_)) {
        LOGE("ensureSourceImage(%u,%u) failed", stagedW_, stagedH_);
        draw = false;
    }

    // The image ultimately blitted into the swapchain, and its region.
    VkImage blitSrc = VK_NULL_HANDLE;
    int32_t srcW = 0, srcH = 0;

    if (draw) {
        // Upload staging -> source image.
        imageBarrier(cmd, srcImage_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = stagedW_;    // tightly packed
        region.bufferImageHeight = stagedH_;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {stagedW_, stagedH_, 1};
        vkCmdCopyBufferToImage(cmd, staging_, srcImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        if (shaderChain_ && ensureShaderOutput((uint32_t)outW, (uint32_t)outH)) {
            // Shader path: srcImage (SHADER_READ_ONLY) -> librashader -> shaderOut,
            // then shaderOut is blitted into the swapchain output rect below.
            imageBarrier(cmd, srcImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

            libra_image_vk_t in{srcImage_, kSrcFormat, stagedW_, stagedH_};
            libra_image_vk_t out{shaderOut_, kSrcFormat, shaderOutW_, shaderOutH_};
            // Render the final pass across the WHOLE output image. A null viewport
            // left librashader drawing the game at a fraction of the target, so it
            // presented tiny; the explicit viewport makes the shaded frame fill it
            // (and lets CRT-style passes work at true output resolution).
            libra_viewport_t vp{0.0f, 0.0f, shaderOutW_, shaderOutH_};
            libra_error_t err = libra_vk_filter_chain_frame(
                &shaderChain_, cmd, frameCount_++, in, out,
                &vp, /*mvp*/ nullptr, /*opt*/ nullptr);
            if (err) {
                logLibraError("libra_vk_filter_chain_frame — falling back to passthrough", err);
                // Fall back: blit the raw source this frame.
                imageBarrier(cmd, srcImage_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                blitSrc = srcImage_; srcW = (int32_t)stagedW_; srcH = (int32_t)stagedH_;
            } else {
                // librashader leaves its output in SHADER_READ_ONLY_OPTIMAL; move
                // it to TRANSFER_SRC for the blit to the swapchain.
                imageBarrier(cmd, shaderOut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                blitSrc = shaderOut_; srcW = (int32_t)shaderOutW_; srcH = (int32_t)shaderOutH_;
            }
        } else {
            // Passthrough: source image straight to the swapchain.
            imageBarrier(cmd, srcImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            blitSrc = srcImage_; srcW = (int32_t)stagedW_; srcH = (int32_t)stagedH_;
        }
    }

    // Swapchain image -> TRANSFER_DST, cleared to black (letterbox bars).
    imageBarrier(cmd, swap, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    VkImageSubresourceRange full{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &full);

    if (draw && blitSrc) {
        // Order the clear before the blit (both TRANSFER writes to `swap`).
        imageBarrier(cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {srcW, srcH, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {outX, outY, 0};
        blit.dstOffsets[1] = {outX + outW, outY + outH, 1};
        vkCmdBlitImage(cmd, blitSrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }

    // Swapchain image -> PRESENT.
    imageBarrier(cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    VK_CHECK(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &imageAvailable_[frame_];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderFinished_[frame_];
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, inFlight_[frame_]), "vkQueueSubmit");
    return true;
}

// ---------------------------------------------------------------------------
// Screenshot — straight from the mapped staging buffer (BGRA -> RGBA)
// ---------------------------------------------------------------------------

bool VulkanRenderer::screenshotRaw(std::vector<uint8_t>& rgbaOut, uint32_t& w, uint32_t& h) {
    if (!hasFrame_ || !stagingMapped_ || stagedW_ == 0 || stagedH_ == 0) return false;
    w = stagedW_;
    h = stagedH_;
    rgbaOut.resize((size_t)w * h * 4);
    const uint8_t* src = static_cast<const uint8_t*>(stagingMapped_);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        rgbaOut[i * 4 + 0] = src[i * 4 + 2];  // R <- B
        rgbaOut[i * 4 + 1] = src[i * 4 + 1];  // G
        rgbaOut[i * 4 + 2] = src[i * 4 + 0];  // B <- R
        rgbaOut[i * 4 + 3] = src[i * 4 + 3];  // A
    }
    return true;
}

// ---------------------------------------------------------------------------
// librashader filter chain
// ---------------------------------------------------------------------------

bool VulkanRenderer::ensureShaderOutput(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return false;
    if (shaderOut_ && shaderOutW_ == w && shaderOutH_ == h) return true;
    if (device_) vkDeviceWaitIdle(device_);
    if (shaderOut_)    { vkDestroyImage(device_, shaderOut_, nullptr);   shaderOut_ = VK_NULL_HANDLE; }
    if (shaderOutMem_) { vkFreeMemory(device_, shaderOutMem_, nullptr);  shaderOutMem_ = VK_NULL_HANDLE; }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kSrcFormat;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &shaderOut_) != VK_SUCCESS) { shaderOut_ = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements mreq{};
    vkGetImageMemoryRequirements(device_, shaderOut_, &mreq);
    uint32_t type = findMemoryType(mreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mreq.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &mai, nullptr, &shaderOutMem_) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, shaderOut_, shaderOutMem_, 0);
    shaderOutW_ = w;
    shaderOutH_ = h;
    return true;
}

void VulkanRenderer::destroyShaderChain() {
    if (device_) vkDeviceWaitIdle(device_);
    if (shaderChain_) { libra_vk_filter_chain_free(&shaderChain_); shaderChain_ = nullptr; }
    if (shaderOut_)    { vkDestroyImage(device_, shaderOut_, nullptr);   shaderOut_ = VK_NULL_HANDLE; }
    if (shaderOutMem_) { vkFreeMemory(device_, shaderOutMem_, nullptr);  shaderOutMem_ = VK_NULL_HANDLE; }
    shaderOutW_ = shaderOutH_ = 0;
}

bool VulkanRenderer::setShader(const char* path) {
    destroyShaderChain();
    if (!path || path[0] == '\0') {
        LOGI("shader cleared (passthrough)");
        return true;
    }
    libra_shader_preset_t preset = nullptr;
    libra_error_t err = libra_preset_create(path, &preset);
    if (err) {
        logLibraError("libra_preset_create", err);
        return false;
    }

    libra_device_vk_t vk{};
    vk.physical_device = physical_;
    vk.instance = instance_;
    vk.device = device_;
    vk.queue = queue_;
    vk.entry = vkGetInstanceProcAddr;

    filter_chain_vk_opt_t opt{};
    opt.version = LIBRASHADER_CURRENT_VERSION;
    // librashader sizes its internal descriptor pools by frames_in_flight; its
    // own default is 3 and passing our swapchain's 2 tripped ERROR_OUT_OF_POOL_MEMORY
    // on Adreno. This is independent of our swapchain frame count.
    opt.frames_in_flight = 3;
    opt.force_no_mipmaps = false;
    opt.use_dynamic_rendering = false;  // render-pass mode: widest device support
    opt.disable_cache = false;

    err = libra_vk_filter_chain_create(&preset, vk, &opt, &shaderChain_);
    if (err) {
        logLibraError("libra_vk_filter_chain_create", err);
        libra_preset_free(&preset);
        shaderChain_ = nullptr;
        return false;
    }
    frameCount_ = 0;
    LOGI("shader loaded: %s", path);
    return true;
}

// ---------------------------------------------------------------------------

VulkanRenderer::~VulkanRenderer() {
    if (device_) vkDeviceWaitIdle(device_);
    destroyShaderChain();
    destroySwapchain();
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (window_) ANativeWindow_release(window_);
    if (stagingMapped_) vkUnmapMemory(device_, stagingMemory_);
    if (staging_) vkDestroyBuffer(device_, staging_, nullptr);
    if (stagingMemory_) vkFreeMemory(device_, stagingMemory_, nullptr);
    if (srcImage_) vkDestroyImage(device_, srcImage_, nullptr);
    if (srcMemory_) vkFreeMemory(device_, srcMemory_, nullptr);
    for (int i = 0; i < kFramesInFlight; i++) {
        if (imageAvailable_[i]) vkDestroySemaphore(device_, imageAvailable_[i], nullptr);
        if (renderFinished_[i]) vkDestroySemaphore(device_, renderFinished_[i], nullptr);
        if (inFlight_[i]) vkDestroyFence(device_, inFlight_[i], nullptr);
    }
    if (cmdPool_) vkDestroyCommandPool(device_, cmdPool_, nullptr);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}
