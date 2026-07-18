#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Plain-C face over librashader's Metal runtime. The real header's Metal API
// is guarded by __OBJC__ and takes id<MTL…> types, so it is not callable from
// Swift through a clang module — this shim (librashader_metal_shim.mm) bridges
// it. Metal objects cross as void* (__bridge'd ObjC ids).

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lrs_mtl_chain lrs_mtl_chain;

// Build a filter chain from a .slangp preset on the given command queue
// (id<MTLCommandQueue>). NULL on any failure (bad preset, ABI mismatch);
// the cause is logged.
lrs_mtl_chain* lrs_mtl_chain_create(const char* preset_path, void* command_queue);

// Run the chain: input/output are id<MTLTexture>, command_buffer is
// id<MTLCommandBuffer> with no prior encoders. The viewport is the region of
// `output` to render into. False on failure (logged) — caller falls back to
// the raw source for this frame.
bool lrs_mtl_chain_frame(lrs_mtl_chain* chain, void* command_buffer,
                         size_t frame_count,
                         void* input_texture, void* output_texture,
                         float x, float y, uint32_t width, uint32_t height);

void lrs_mtl_chain_free(lrs_mtl_chain* chain);

#ifdef __cplusplus
}
#endif
