// Obj-C++ bridge to librashader's Metal runtime — see librashader_metal_shim.h
// for why this exists. Mirrors the Android Vulkan path's conventions
// (vulkan_renderer.cpp): preset ownership passes to the chain on success,
// failures log through libra_error_write and fall back to passthrough.

#import <Metal/Metal.h>

#define LIBRA_RUNTIME_METAL 1
#include "librashader.h"

#include "librashader_metal_shim.h"

#include <cstdio>
#include <cstdlib>

struct lrs_mtl_chain {
    libra_mtl_filter_chain_t chain = nullptr;
};

// librashader's glslang objects are compiled by their crate against the host
// SDK's newest libc++ headers, which call the out-of-line libc++ symbol
// std::__1::__hash_memory. Runtimes older than the SDK don't export it and
// the app dies at dyld ("Symbol missing"). Provide the symbol locally under
// its mangled name (the header's own declaration forbids an in-namespace
// definition). Only glslang's internal hash containers bind to it, so any
// well-distributed byte hash is semantically sound; two-level namespace
// binding keeps system libraries on the system copy. MurmurHash2 64-bit —
// the algorithm libc++ itself uses on 64-bit targets.
extern "C" size_t _ZNSt3__113__hash_memoryEPKvm(const void* key, size_t len) noexcept {
    const uint64_t m = 0xc6a4a7935bd1e995ull;
    const int r = 47;
    uint64_t h = 0xc70f6907ull ^ (len * m);
    const unsigned char* data = (const unsigned char*)key;
    const unsigned char* end = data + (len & ~7ull);
    while (data != end) {
        uint64_t k;
        memcpy(&k, data, 8);
        data += 8;
        k *= m; k ^= k >> r; k *= m;
        h ^= k; h *= m;
    }
    switch (len & 7) {
        case 7: h ^= (uint64_t)data[6] << 48; [[fallthrough]];
        case 6: h ^= (uint64_t)data[5] << 40; [[fallthrough]];
        case 5: h ^= (uint64_t)data[4] << 32; [[fallthrough]];
        case 4: h ^= (uint64_t)data[3] << 24; [[fallthrough]];
        case 3: h ^= (uint64_t)data[2] << 16; [[fallthrough]];
        case 2: h ^= (uint64_t)data[1] << 8;  [[fallthrough]];
        case 1: h ^= (uint64_t)data[0];
                h *= m;
    }
    h ^= h >> r; h *= m; h ^= h >> r;
    return (size_t)h;
}

static void logLibraError(const char* what, libra_error_t err) {
    char* msg = nullptr;
    if (libra_error_write(err, &msg) == 0 && msg) {
        fprintf(stderr, "[RetroEmulator] %s: %s\n", what, msg);
        libra_error_free_string(&msg);
    } else {
        fprintf(stderr, "[RetroEmulator] %s: unknown librashader error\n", what);
    }
    libra_error_free(&err);
}

extern "C" {

lrs_mtl_chain* lrs_mtl_chain_create(const char* preset_path, void* command_queue) {
    if (!preset_path || !command_queue) return nullptr;
    if (libra_instance_abi_version() != LIBRASHADER_CURRENT_ABI) {
        fprintf(stderr, "[RetroEmulator] librashader ABI mismatch — header %d, library %zu\n",
                LIBRASHADER_CURRENT_ABI, libra_instance_abi_version());
        return nullptr;
    }

    libra_shader_preset_t preset = nullptr;
    if (libra_error_t err = libra_preset_create(preset_path, &preset)) {
        logLibraError("libra_preset_create", err);
        return nullptr;
    }

    filter_chain_mtl_opt_t opt{};
    opt.version = LIBRASHADER_CURRENT_VERSION;
    opt.force_no_mipmaps = false;

    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)command_queue;
    libra_mtl_filter_chain_t chain = nullptr;
    if (libra_error_t err = libra_mtl_filter_chain_create(&preset, queue, &opt, &chain)) {
        logLibraError("libra_mtl_filter_chain_create", err);
        libra_preset_free(&preset);
        return nullptr;
    }

    auto* out = new lrs_mtl_chain{};
    out->chain = chain;
    return out;
}

bool lrs_mtl_chain_frame(lrs_mtl_chain* handle, void* command_buffer,
                         size_t frame_count,
                         void* input_texture, void* output_texture,
                         float x, float y, uint32_t width, uint32_t height) {
    if (!handle || !handle->chain || !command_buffer || !input_texture || !output_texture) {
        return false;
    }
    id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)command_buffer;
    id<MTLTexture> input  = (__bridge id<MTLTexture>)input_texture;
    id<MTLTexture> output = (__bridge id<MTLTexture>)output_texture;

    libra_viewport_t viewport{};
    viewport.x = x;
    viewport.y = y;
    viewport.width = width;
    viewport.height = height;

    if (libra_error_t err = libra_mtl_filter_chain_frame(
            &handle->chain, cmd, frame_count, input, output,
            &viewport, /*mvp*/ nullptr, /*opt*/ nullptr)) {
        logLibraError("libra_mtl_filter_chain_frame — falling back to passthrough", err);
        return false;
    }
    return true;
}

void lrs_mtl_chain_free(lrs_mtl_chain* handle) {
    if (!handle) return;
    if (handle->chain) {
        if (libra_error_t err = libra_mtl_filter_chain_free(&handle->chain)) {
            logLibraError("libra_mtl_filter_chain_free", err);
        }
    }
    delete handle;
}

} // extern "C"
