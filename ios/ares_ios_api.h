#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque context — one instance per emulated system.
// The current implementation is backed by a global singleton; do not create
// more than one context concurrently.
typedef struct AresContext AresContext;

// Lifecycle --------------------------------------------------------------------

AresContext* ares_create(void);
void         ares_destroy(AresContext* ctx);

// System / ROM loading --------------------------------------------------------

// Load the SFC system core.
// ipl_rom: 64-byte SPC700 boot ROM (may be NULL — SMP won't boot, audio hangs).
// boards_bml: required BML database for memory-map lookup.
bool ares_load_system(AresContext* ctx,
                      const uint8_t* ipl_rom, size_t ipl_size,
                      const uint8_t* boards_bml, size_t bml_size);

// Load and power a Super Famicom ROM image.  Returns false on bad header.
bool ares_load_rom(AresContext* ctx, const uint8_t* rom, size_t rom_size);

// Emulation control -----------------------------------------------------------

// Tick one video frame.  Returns false when no ROM is loaded or when paused.
bool ares_tick(AresContext* ctx);

// Write the button bitmask for port 1 or 2 (bits match iOS EmulatorInput).
void ares_set_input(AresContext* ctx, int port, uint32_t bits);

void ares_pause(AresContext* ctx);
void ares_resume(AresContext* ctx);

// Video -----------------------------------------------------------------------

// Copy the latest frame into caller-supplied ARGB8888 buffer.
// out_width / out_height set to 0 when no frame is available.
// Returns false when no frame is ready.
bool ares_get_frame(AresContext* ctx,
                    uint32_t* out_buf, size_t buf_capacity,
                    uint32_t* out_width, uint32_t* out_height);

// Audio -----------------------------------------------------------------------

// Drain mixed stereo float samples (interleaved L/R) into caller buffer.
// Returns the number of floats written.
size_t ares_read_audio(AresContext* ctx, float* out, size_t capacity);

// State save / load -----------------------------------------------------------

bool ares_state_save(AresContext* ctx, const char* path);
bool ares_state_load(AresContext* ctx, const char* path);

// Memory access (WRAM bus addresses 0x7E0000–0x7FFFFF only) ------------------

// Returns bytes written, or -1 on error.
int  ares_read_memory(AresContext* ctx, uint32_t address, uint8_t* out, int length);
void ares_write_memory(AresContext* ctx, uint32_t address,
                       const uint8_t* bytes, int length);

// Metadata --------------------------------------------------------------------

const char* ares_get_region(AresContext* ctx);     // "NTSC", "PAL", or ""
const char* ares_get_ports_json(AresContext* ctx); // JSON array of port objects

#ifdef __cplusplus
}
#endif
