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
typedef struct EmuContext EmuContext;

// Lifecycle --------------------------------------------------------------------

EmuContext* emu_create(void);
void         emu_destroy(EmuContext* ctx);

// Tear down any running core and reset the context to factory state — the
// Stop semantics (Android cycles destroy()+init(); this keeps the pointer
// stable for the host's stored references).  Flush saves first if wanted.
// Emulation thread only.
void emu_reset(EmuContext* ctx);

// System / ROM loading --------------------------------------------------------

// STAGE a system declaration by ares id ("fc", "sfc", "gb", "gbc", "gba", "md")
// — no core boots until emu_load_rom arrives with a ROM (every boot is
// ROM-first so the region variant is always right).  Re-staging over a running
// core is legal; the running game continues until the next emu_load_rom.
// System firmware (SFC ipl.rom + boards.bml, GB/GBC boot ROM, MD TMSS, GBA open
// BIOS) is embedded in the library — no assets required.  Returns
// false for ids not compiled into this build.
// bios_path: optional real BIOS dump to override gba's embedded open BIOS;
// NULL/empty otherwise (all systems boot on embedded firmware).
// backend: engine to serve this system ("ares", "sameboy", …); NULL/empty
// picks the bundled fast core where one exists. An explicitly requested
// engine that does not claim the system fails the staging — never a silent
// substitution.
bool emu_load_system(EmuContext* ctx, const char* system_id, const char* bios_path,
                     const char* backend);

// Comma-separated ids of the systems compiled into this build, e.g.
// "fc,gb,gba,gbc,md,sfc".  Static storage — do not free.
const char* emu_supported_systems(void);

// The engine serving calls right now: active, else staged, else "".
// Thread-local storage — copy before the next call on the same thread.
const char* emu_get_backend_name(EmuContext* ctx);

// Per-system engine availability + the fast-by-default pick, as a JSON
// object string:
// {"gb":{"backends":["ares","sameboy"],"default":"sameboy"}, …}.
// Thread-local storage — copy before the next call on the same thread.
const char* emu_get_backends_json(void);

// Capability gates, answered against the engine a call would actually hit
// (active first, else staged) — the bridge rejects an unsupported option
// loudly instead of letting it become a silent no-op.
bool emu_video_settings_supported(EmuContext* ctx);
bool emu_toggle_supported(EmuContext* ctx, const char* key);

// Engine-declared options (libretro core options). Set returns "" when
// applied, else the refusal message the bridge surfaces as
// UNSUPPORTED_OPTION; `staged` targets the engine the next boot uses. The
// schema JSON lists [{"key","choices":[…],"default","current"}, …].
// Thread-local storage — copy before the next call on the same thread.
const char* emu_set_engine_option(EmuContext* ctx, const char* key,
                                  const char* value, bool staged);
const char* emu_get_engine_options_json(EmuContext* ctx);

// Comma-separated ROM file extensions (no dots) valid for a system id —
// the LoadRom family-mismatch gate.  Empty string for unknown ids.
// Thread-local storage — copy before the next call on the same thread.
const char* emu_system_extensions(EmuContext* ctx, const char* system_id);

// Boot the staged system with this ROM — the ONE boot path, first load and
// every swap alike.  Analyzes the ROM, resolves the region variant like
// desktop-ares (ROM region list × preferred_regions CSV, region_override
// wins; empty/NULL = defaults), tears down any running core, boots fresh.
// save_prefix: battery-save location — files are written as
// "<prefix>.save.ram", "<prefix>.save.eeprom", etc., and existing files seed
// the cartridge before boot.  NULL disables persistence.
// Returns 1 on success; 0 when the ROM was rejected BEFORE any teardown (a
// running game is untouched); -1 when a later failure left the emulator
// cleanly stopped; -2 when the system requires firmware and no biosPath was
// staged (pre-teardown, running game untouched).
int emu_load_rom(EmuContext* ctx, const uint8_t* rom, size_t rom_size,
                  const char* save_prefix,
                  const char* region_override, const char* preferred_regions);

// Stage a slotted-media ROM (SuFami Turbo: index 0 = Slot A, 1 = Slot B;
// BS-X: index 0 = the BS Memory slot) to be inserted at the next
// emu_load_rom, whose base ROM must be the slot-carrying cartridge.
// NULL/0 bytes clear the slot.  Emulation thread only.
void emu_stage_slot(EmuContext* ctx, int index, const uint8_t* rom, size_t rom_size);

// Test seam: whether a staged slot cartridge actually connected at load.
bool emu_is_slot_connected(EmuContext* ctx, int index);

// Stage a boot option by ares' own option() name (e.g. "Pixel Accuracy",
// value "true"/"false"). Applied before the next boot's load() — boot options
// pick renderer implementations, so a running core never changes; cores
// without the option ignore it.
void emu_stage_boot_option(EmuContext* ctx, const char* name, const char* value);

// Live boot-option value from the running core — "true"/"false", or "" when
// nothing is loaded or the core doesn't expose it. Reads core state (which
// SNES PPU is bound), not the staged map. Pointer valid until the next call.
const char* emu_get_boot_option(EmuContext* ctx, const char* name);

// Write current battery-backed memory to disk under the prefix passed to
// emu_load_rom.  Must run on the emulation thread.  Returns false when
// nothing was persisted.
bool emu_flush_saves(EmuContext* ctx);

// Emulation control -----------------------------------------------------------

// Tick one video frame.  Returns false when no ROM is loaded or when paused.
bool emu_tick(EmuContext* ctx);

// Write the HARDWARE button bitmask for a logical port (1-based, up to 5 —
// multitap territory).  OR'd with the software mask at poll time; bits match
// iOS EmulatorInput / the positional gamepad.
void emu_set_input(EmuContext* ctx, int port, uint32_t bits);

// Read back the combined (hardware | software) bitmask the core will see on
// its next input poll.  Test/diagnostic seam; 0 for unknown port/context.
uint32_t emu_get_input(EmuContext* ctx, int port);

// Input — device-handle model.  Controllers are registered explicitly, never
// auto-allocated: a boot with no registrations has no input.  Status-string
// returns: "" on success, else a category-A code ("SYSTEM_NOT_LOADED",
// "INVALID_PARAMETERS", "UNSUPPORTED_DEVICE", "UNKNOWN_BUTTON:<name>").
// Thread-local storage — copy before the next call on the same thread. -------

// Register (or swap) the device on a PHYSICAL port; empty device disconnects.
// Validated against the system registry by id (not the live core, which may
// still be staging).  The allocate/connect + cache rebuild is deferred to the
// emulation thread (consumed at the top of emu_tick).  Registrations persist
// across emu_load_rom.
const char* emu_connect_device(EmuContext* ctx, const char* system_id,
                                int port, const char* device);

// The LOGICAL port numbers a physical port's registered device occupies — one
// for a normal controller, four for a Super Multitap.  Writes up to capacity
// entries into out; returns the count.
int emu_device_ports(EmuContext* ctx, const char* system_id,
                      int physical, int* out, int capacity);

// Set or clear one SOFTWARE button on a logical port, resolved against the
// connected device's own button set.  Merges with the hardware mask.
const char* emu_press_button(EmuContext* ctx, int port,
                              const char* name, bool down);

// Accumulate a relative delta on one axis of the connected device (mouse /
// light-gun X/Y).  Consumed on the core's next poll.
const char* emu_set_axis(EmuContext* ctx, int port, const char* name, int value);

// Aim a light-gun at an absolute normalized position (0..1).  ares' guns are
// relative-only, so a shadow cursor mirrors the gun's internal cursor and the
// needed delta is fed to reach the target.
const char* emu_aim_at(EmuContext* ctx, int port, float nx, float ny);

// Merge a per-port controller remap: emulated[i] (a core button name) reads
// the positional slot of source[i].  count == 0 resets the port to defaults.
// Validated wholesale before mutating; applied on the emulation thread.
const char* emu_set_input_mapping(EmuContext* ctx, int port,
                                   const char* const* emulated,
                                   const char* const* source, int count);

// Test seam: the positional bit a core button currently reads (post-remap),
// or -1 when nothing is cached for (port, name).
int emu_get_button_bit(EmuContext* ctx, int port, const char* name);

// Test seam: the pending (unconsumed) accumulated delta on one axis.
int emu_get_axis_accum(EmuContext* ctx, int port, const char* name);

void emu_pause(EmuContext* ctx);
void emu_resume(EmuContext* ctx);

// Audio / video options ---------------------------------------------------------

// Master volume (0–1) and stereo balance (−1 left … +1 right).  Any thread.
void emu_set_audio(EmuContext* ctx, float volume, float balance);

// Global display settings on the ares screen node.  Ranges follow ares:
// luminance/saturation 0–1, gamma 1.0–2.0.  overscan false trims the
// borders (our default); true shows the full overscan canvas.
// Emulation thread only.
void emu_set_video(EmuContext* ctx, float luminance, float saturation,
                    float gamma, bool color_bleed, bool overscan);

// Per-system emulation toggle by wrapper key (colorEmulation, deepBlackBoost,
// interframeBlending).  No-ops when the loaded core doesn't declare the node.
// Emulation thread only.
void emu_set_core_boolean(EmuContext* ctx, const char* key, bool value);

// Test seam: a toggle's current node value (1/0), or -1 if absent.
int emu_get_core_boolean(EmuContext* ctx, const char* key);

// Video -----------------------------------------------------------------------

// Copy the latest frame into caller-supplied ARGB8888 buffer.
// out_width / out_height set to 0 when no frame is available.
// Returns false when no frame is ready.
bool emu_get_frame(EmuContext* ctx,
                    uint32_t* out_buf, size_t buf_capacity,
                    uint32_t* out_width, uint32_t* out_height);

// Screen-node presentation geometry for the latest frame:
// out[7] = {width, height, scaleX, scaleY, aspectX, aspectY, rotation}.
// Presentation size follows ares desktop-ui/program/platform.cpp:95-115:
// videoWidth = width·scaleX·aspectX/aspectY, videoHeight = height·scaleY,
// swap when rotation is 90/270, then best-fit into the viewport.
// All zeros before the first frame.  Emulation thread only.
void emu_get_video_geometry(EmuContext* ctx, double out[7]);

// True refresh rate of the loaded system, reported by the core via ares'
// Platform::refreshRateHint (region- and mode-aware: SFC NTSC 60.0988,
// GB 59.7275, PAL ~50). 0.0 until the system powers on. Any thread.
double emu_get_refresh_rate_hint(EmuContext* ctx);

// Audio -----------------------------------------------------------------------

// Drain mixed stereo float samples (interleaved L/R) into caller buffer.
// Returns the number of floats written.
size_t emu_read_audio(EmuContext* ctx, float* out, size_t capacity);

// State save / load -----------------------------------------------------------

bool emu_state_save(EmuContext* ctx, const char* path);
bool emu_state_load(EmuContext* ctx, const char* path);

// Memory access — the work-RAM bus window is system-specific:
// SFC 0x7E0000–0x7FFFFF, FC 0x0000–0x07FF, GB 0xC000–0xDFFF,
// MD 0xFF0000–0xFFFFFF. ----------------------------------------------------

// Returns bytes written, or -1 on error.
int  emu_read_memory(EmuContext* ctx, uint32_t address, uint8_t* out, int length);
void emu_write_memory(EmuContext* ctx, uint32_t address,
                       const uint8_t* bytes, int length);

// Rewind / run-ahead — emulation thread only (state is read inside emu_tick).

// Enable/disable rewind snapshot capture.  buffer_seconds sizes the history
// (6 snapshots per second); <= 0 keeps ares' desktop default (~16.7 s).
// Disabling drops the captured history.
void emu_configure_rewind(EmuContext* ctx, bool enabled, int buffer_seconds);

// Enter/exit rewind playback (5× the capture rate, ares desktop semantics;
// play resumes when history runs out).
// Returns 1 rewinding, 0 playing, -1 rewind capture not enabled.
int emu_toggle_rewind(EmuContext* ctx);

// Enable/disable one-frame run-ahead: each tick runs a hidden frame plus a
// rolled-back visible preview, cutting perceived input latency by one frame
// at 2× emulation cost.  Suppressed during fast-forward and rewind.
void emu_set_run_ahead(EmuContext* ctx, bool enabled);

// Mirror of the host fast-forward flag — suppresses run-ahead.  Any thread.
void emu_set_fast_forward(EmuContext* ctx, bool active);

// Enable/disable dynamic rate control (default on): each tick skews the
// stream resamplers by up to ±0.5% toward a half-full audio ring, so
// production tracks the device DAC clock instead of drifting into overflow
// or underrun.  Any thread.
void emu_set_dynamic_rate_control(EmuContext* ctx, bool enabled);

// Rumble — cores publish motor state (SFC Rumble Gamepad, GB MBC5 rumble
// carts); the host polls per frame and drives its haptics.
// Any thread. ---------------------------------------------------------------

// Gate rumble forwarding.  Disabling zeroes the motor state.
void emu_set_rumble_enabled(EmuContext* ctx, bool enabled);

// Current motor state, packed strong<<16|weak (u16 each; 0 = off).
uint32_t emu_get_rumble_state(EmuContext* ctx);

// Cheats — ares' raw format: hex "ADDR:VALUE" pairs joined with '+'
// (e.g. "7E0010:01+7E0011:FF").  The value overrides every CPU read of the
// address while active.  Cheats clear automatically when a new ROM loads.
// Emulation thread only (the cheat map is read inside emu_tick). ------------

// Register (or replace) a cheat under its exact code string.
// Returns false when no valid ADDR:VALUE pair parses.
bool emu_add_cheat(EmuContext* ctx, const char* code);

// Remove a cheat by exact code string.  Returns false when it wasn't active.
bool emu_remove_cheat(EmuContext* ctx, const char* code);

void emu_clear_cheats(EmuContext* ctx);

// Metadata --------------------------------------------------------------------

const char* emu_get_region(EmuContext* ctx);     // "NTSC", "PAL", or ""
const char* emu_get_ports_json(EmuContext* ctx); // JSON array of port objects
// Comma-joined names of the buttons held on a port (hardware or software).
const char* emu_get_pressed_buttons(EmuContext* ctx, int port);

#ifdef __cplusplus
}
#endif
