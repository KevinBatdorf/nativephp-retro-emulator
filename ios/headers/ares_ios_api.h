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

// Load a system core by ares id ("fc", "sfc", "gb", "md").  System firmware
// (SFC ipl.rom + boards.bml, GB boot ROM, MD TMSS) is embedded in the library
// — no assets required.  Returns false for ids not compiled into this build.
bool ares_load_system(AresContext* ctx, const char* system_id);

// Comma-separated ids of the systems compiled into this build, e.g.
// "fc,sfc,gb,md".  Static storage — do not free.
const char* ares_supported_systems(void);

// Load and power a ROM image for the loaded system.  Returns false on bad header.
// save_prefix: battery-save location — files are written as
// "<prefix>.save.ram", "<prefix>.save.eeprom", etc., and existing files seed
// the cartridge before boot.  NULL disables persistence.
bool ares_load_rom(AresContext* ctx, const uint8_t* rom, size_t rom_size,
                   const char* save_prefix);

// Write current battery-backed memory to disk under the prefix passed to
// ares_load_rom.  Must run on the emulation thread.  Returns false when
// nothing was persisted.
bool ares_flush_saves(AresContext* ctx);

// Emulation control -----------------------------------------------------------

// Tick one video frame.  Returns false when no ROM is loaded or when paused.
bool ares_tick(AresContext* ctx);

// Write the button bitmask for port 1 or 2 (bits match iOS EmulatorInput).
void ares_set_input(AresContext* ctx, int port, uint32_t bits);

void ares_pause(AresContext* ctx);
void ares_resume(AresContext* ctx);

// Audio / video options ---------------------------------------------------------

// Master volume (0–1) and stereo balance (−1 left … +1 right).  Any thread.
void ares_set_audio(AresContext* ctx, float volume, float balance);

// Video post-processing on the ares screen node.  Ranges follow ares:
// luminance/saturation 0–1, gamma 1.0–2.0.  Emulation thread only.
void ares_set_video(AresContext* ctx, float luminance, float saturation,
                    float gamma, bool color_bleed, bool interframe_blending);

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

// Memory access — the work-RAM bus window is system-specific:
// SFC 0x7E0000–0x7FFFFF, FC 0x0000–0x07FF, GB 0xC000–0xDFFF,
// MD 0xFF0000–0xFFFFFF. ----------------------------------------------------

// Returns bytes written, or -1 on error.
int  ares_read_memory(AresContext* ctx, uint32_t address, uint8_t* out, int length);
void ares_write_memory(AresContext* ctx, uint32_t address,
                       const uint8_t* bytes, int length);

// Rewind / run-ahead — emulation thread only (state is read inside ares_tick).

// Enable/disable rewind snapshot capture.  buffer_seconds sizes the history
// (6 snapshots per second); <= 0 keeps ares' desktop default (~16.7 s).
// Disabling drops the captured history.
void ares_configure_rewind(AresContext* ctx, bool enabled, int buffer_seconds);

// Enter/exit rewind playback (5× the capture rate, ares desktop semantics;
// play resumes when history runs out).
// Returns 1 rewinding, 0 playing, -1 rewind capture not enabled.
int ares_toggle_rewind(AresContext* ctx);

// Enable/disable one-frame run-ahead: each tick runs a hidden frame plus a
// rolled-back visible preview, cutting perceived input latency by one frame
// at 2× emulation cost.  Suppressed during fast-forward and rewind.
void ares_set_run_ahead(AresContext* ctx, bool enabled);

// Mirror of the host fast-forward flag — suppresses run-ahead.  Any thread.
void ares_set_fast_forward(AresContext* ctx, bool active);

// Enable/disable dynamic rate control (default on): each tick skews the
// stream resamplers by up to ±0.5% toward a half-full audio ring, so
// production tracks the device DAC clock instead of drifting into overflow
// or underrun.  Any thread.
void ares_set_dynamic_rate_control(AresContext* ctx, bool enabled);

// Rumble — cores publish motor state (SFC Rumble Gamepad, GB MBC5 rumble
// carts, N64 Rumble Pak); the host polls per frame and drives its haptics.
// Any thread. ---------------------------------------------------------------

// Gate rumble forwarding.  Disabling zeroes the motor state.
void ares_set_rumble_enabled(AresContext* ctx, bool enabled);

// Current motor state, packed strong<<16|weak (u16 each; 0 = off).
uint32_t ares_get_rumble_state(AresContext* ctx);

// Cheats — ares' raw format: hex "ADDR:VALUE" pairs joined with '+'
// (e.g. "7E0010:01+7E0011:FF").  The value overrides every CPU read of the
// address while active.  Cheats clear automatically when a new ROM loads.
// Emulation thread only (the cheat map is read inside ares_tick). ------------

// Register (or replace) a cheat under its exact code string.
// Returns false when no valid ADDR:VALUE pair parses.
bool ares_add_cheat(AresContext* ctx, const char* code);

// Remove a cheat by exact code string.  Returns false when it wasn't active.
bool ares_remove_cheat(AresContext* ctx, const char* code);

void ares_clear_cheats(AresContext* ctx);

// Metadata --------------------------------------------------------------------

const char* ares_get_region(AresContext* ctx);     // "NTSC", "PAL", or ""
const char* ares_get_ports_json(AresContext* ctx); // JSON array of port objects

#ifdef __cplusplus
}
#endif
