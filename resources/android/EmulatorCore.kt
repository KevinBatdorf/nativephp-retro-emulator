package com.kevinbatdorf.plugins.retroemulator

import android.view.Surface

/**
 * JNI wrapper around the ares emulator core.
 *
 * Threading: [init], [loadSystem], [loadRom], [tick], and [destroy] MUST be
 * called from the same thread (the render thread) because the ares scheduler
 * uses libco coroutines that capture their stack context at [loadSystem] time.
 * The Vulkan surface calls ([surfaceCreated]/[presentFrame]/…) run on that same
 * render thread.
 *
 * Lifecycle:
 *   init() → surfaceCreated() → loadSystem() → loadRom() → tick()/presentFrame() … → destroy()
 */
class EmulatorCore {

    companion object {
        // loadRom result codes — see nativeLoadRom in ares_jni.cpp.
        const val LOAD_OK = 1                  // booted and running
        const val LOAD_REJECTED = 0            // rejected pre-teardown; prior game untouched
        const val LOAD_FAILED_STOPPED = -1     // failed after teardown; emulator cleanly stopped
    }

    init {
        // Load the librashader Vulkan runtime first — libretro_emulator.so links
        // it (NEEDED liblibrashader.so, resolved from jniLibs). Loading it
        // explicitly first makes a missing/renamed lib fail loudly here.
        System.loadLibrary("librashader")
        System.loadLibrary("retro_emulator")
    }

    /** Initialize the ares platform singleton. Returns false if already initialized. */
    fun init(): Boolean = nativeInit()

    /** Tear down the ares platform singleton and free all emulator state. */
    fun destroy() = nativeDestroy()

    /** Returns the ares version string (e.g. "v140-android"). */
    fun version(): String = nativeVersion()

    /**
     * Bind an Android [Surface] and build the Vulkan swapchain. Render thread only.
     * Brings up the Vulkan device on first call; survives core stop/init.
     */
    fun surfaceCreated(surface: Surface) = nativeSurfaceCreated(surface)

    /** Recreate the swapchain for a new surface size. Render thread only. */
    fun surfaceChanged(width: Int, height: Int) = nativeSurfaceChanged(width, height)

    /** Destroy the swapchain + surface (device survives). Render thread only. */
    fun surfaceDestroyed() = nativeSurfaceDestroyed()

    /** Tear down the Vulkan device + instance. Call once as the render thread exits. */
    fun releaseRenderer() = nativeReleaseRenderer()

    /**
     * Upload the latest frame and present it, letterboxed into the given output
     * rect (surface pixels), via Vulkan. Blocks on vsync (FIFO). Render thread only.
     */
    fun presentFrame(outX: Int, outY: Int, outW: Int, outH: Int): Boolean =
        nativePresentFrame(outX, outY, outW, outH)

    /**
     * Apply a librashader `.slangp` preset by absolute path; null/empty clears it
     * (passthrough). Returns false on a load/creation error. Render thread only.
     */
    fun setShader(path: String?): Boolean = nativeSetShader(path)

    /**
     * The PRESENTED frame as raw RGBA8 bytes (row-major, width×height×4), or
     * null if no frame yet. With an active shader this is the post-shader
     * output — sized by the output rect, not the core frame — so dimensions
     * come back in [dims] (w, h), not [getFrameWidth]/[getFrameHeight].
     * Render thread only.
     */
    fun screenshotRGBA(dims: IntArray = IntArray(2)): ByteArray? = nativeScreenshotRGBA(dims)

    /**
     * STAGE a system declaration — no core boots until [loadRom] arrives with
     * a ROM (every boot is ROM-first so the region variant is always right).
     * Re-staging over a running core is legal; the running game continues
     * until the next [loadRom]. GL thread only.
     *
     * System firmware (SFC ipl.rom + boards.bml, GB boot ROM, MD TMSS, and the
     * embedded open GBA BIOS) ships in the native library — no assets needed.
     *
     * @param systemId One of [supportedSystems] (e.g. "sfc", "fc", "gb", "md").
     * @param biosPath Optional real BIOS dump to override gba's embedded open
     *                 BIOS for accuracy; null otherwise.
     * @param backend  Engine to serve this system ("ares", "sameboy", …);
     *                 null picks the bundled fast core where one exists.
     */
    fun loadSystem(systemId: String, biosPath: String? = null, backend: String? = null): Boolean =
        nativeLoadSystem(systemId, biosPath ?: "", backend ?: "")

    /** Comma-separated system IDs available in this build (e.g. "fc,gb,gba,gbc,md,sfc"). */
    fun supportedSystems(): String = nativeGetSupportedSystems()

    /** Comma-separated ROM file extensions (no dots) valid for [systemId]. */
    fun systemExtensions(systemId: String): String = nativeGetSystemExtensions(systemId)

    /** The engine serving calls right now: active, else staged, else "". */
    fun backendName(): String = nativeGetBackendName()

    /** Per-system engine availability + default pick, as a JSON object string. */
    fun backendsJson(): String = nativeGetBackendsJson()

    /** Whether the staged/active engine exposes the setVideo settings door. */
    fun videoSettingsSupported(): Boolean = nativeVideoSettingsSupported()

    /** Whether the staged/active engine declares the per-system toggle [key]. */
    fun toggleSupported(key: String): Boolean = nativeToggleSupported(key)

    /**
     * Set an engine-declared option (libretro core options). Legal only when
     * the core declares both the key and the value; behavior is the core
     * author's. Returns "" on success, else the refusal message. [staged]
     * targets the engine the next boot uses; call on the render thread —
     * the running core reads these between frames.
     */
    fun setEngineOption(key: String, value: String, staged: Boolean): String =
        nativeSetEngineOption(key, value, staged)

    /** The engine-declared option schema: [{key, choices, default, current}]. */
    fun engineOptionsJson(): String = nativeGetEngineOptionsJson()

    /**
     * Boot the staged system with this ROM — the ONE boot path, first load and
     * every swap alike. Analyzes the ROM, resolves the region variant like
     * desktop-ares (ROM region list × preference, override wins), tears down
     * any running core, and boots fresh. A ROM that fails analysis leaves a
     * running game untouched. Must be called after [loadSystem] staged a
     * system, from the GL thread.
     *
     * @param romBytes         Raw ROM bytes. A 512-byte copier header is
     *                         stripped automatically if detected.
     * @param savePrefix       Battery-save location prefix — files are written
     *                         as "<prefix>.save.ram", "<prefix>.save.eeprom",
     *                         etc. Existing files seed the cartridge before
     *                         boot. Null disables persistence.
     * @param region           Explicit region override (e.g. "PAL"); empty
     *                         resolves from the ROM analysis.
     * @param preferredRegions Preference order for multi-region ROMs, CSV
     *                         (e.g. "NTSC-J,PAL"); empty uses desktop's
     *                         default "NTSC-U".
     * @return [LOAD_OK] on success; [LOAD_REJECTED] when the ROM failed before
     *         any teardown (a running game is untouched); [LOAD_FAILED_STOPPED]
     *         when a later failure left the emulator cleanly stopped.
     */
    fun loadRom(
        romBytes: ByteArray,
        savePrefix: String? = null,
        region: String = "",
        preferredRegions: String = "",
    ): Int = nativeLoadRom(romBytes, savePrefix, region, preferredRegions)

    /**
     * Stage a Sufami Turbo slot ROM (index 0 = Slot A, 1 = Slot B) to insert at
     * the next [loadRom], whose base must be the ST-LOROM SuFami BIOS. Empty
     * bytes clear the slot. Call before [loadRom].
     */
    fun stageSlot(index: Int, rom: ByteArray) = nativeStageSlot(index, rom)

    /** Test seam: whether a Sufami Turbo slot cartridge actually connected at load. */
    fun isSlotConnected(index: Int): Boolean = nativeIsSlotConnected(index)

    /**
     * Stage a boot option by ares' own option() name (e.g. "Pixel Accuracy").
     * Applied before the next boot's load() — these pick renderer
     * implementations, so a running core never changes; cores without the
     * option ignore it.
     */
    fun stageBootOption(name: String, value: Boolean) =
        nativeStageBootOption(name, if (value) "true" else "false")

    /**
     * Live boot-option value from the running core ("true"/"false"), or ""
     * when nothing is loaded or the core doesn't expose it. Reads core state,
     * not the staged map.
     */
    fun bootOption(name: String): String = nativeGetBootOption(name)

    /**
     * Write current battery-backed memory to disk under the prefix passed to
     * [loadRom]. GL thread only. Returns false when nothing was persisted.
     */
    fun flushSaves(): Boolean = nativeFlushSaves()

    /** Master volume (0–1) and stereo balance (−1 left … +1 right). Any thread. */
    fun setAudio(volume: Float, balance: Float) = nativeSetAudio(volume, balance)

    /**
     * Video post-processing on the ares screen node. Ranges follow ares:
     * luminance/saturation 0–1, gamma 1.0–2.0. overscan false trims the
     * borders (our default); true shows the full overscan canvas.
     * GL thread only.
     */
    fun setVideo(
        luminance: Float,
        saturation: Float,
        gamma: Float,
        colorBleed: Boolean,
        overscan: Boolean,
    ) = nativeSetVideo(luminance, saturation, gamma, colorBleed, overscan)

    /** Apply a per-system emulation toggle; no-ops if the core lacks the node. */
    fun setCoreBoolean(key: String, value: Boolean) = nativeSetCoreBoolean(key, value)

    /** Test seam: a toggle's current node value (1/0), or -1 if absent. */
    fun getCoreBoolean(key: String): Int = nativeGetCoreBoolean(key)

    /**
     * Run one emulated frame. The video callback buffers the frame natively; the
     * render thread uploads + presents it via [presentFrame]. No-op if no ROM is
     * loaded.
     */
    fun tick() = nativeTick()

    /** Width of the most recently rendered frame in pixels (0 before first frame). */
    fun getFrameWidth(): Int = nativeGetFrameWidth()

    /** Height of the most recently rendered frame in pixels (0 before first frame). */
    fun getFrameHeight(): Int = nativeGetFrameHeight()

    /**
     * Screen-node presentation geometry for the latest frame:
     * `[width, height, scaleX, scaleY, aspectX, aspectY, rotation]`.
     * All zeros before the first frame. Safe from any thread.
     */
    fun getVideoGeometry(): DoubleArray = nativeGetVideoGeometry()

    /**
     * True refresh rate of the loaded system, reported by the core via ares'
     * Platform::refreshRateHint (region- and mode-aware: SFC NTSC 60.0988,
     * GB 59.7275, PAL ~50). 0.0 until the system powers on. Any thread.
     */
    fun refreshRateHint(): Double = nativeGetRefreshRateHint()

    /**
     * Drain mixed stereo audio samples from the native ring buffer into
     * [buffer] (interleaved L/R float pairs, range −1..+1).
     *
     * @return Number of floats written (always even; 0 if nothing pending).
     *         May be called from any thread.
     */
    fun readAudio(buffer: FloatArray): Int = nativeReadAudio(buffer)

    /**
     * Write the current button bitmask for one controller port. Safe to call
     * from any thread. Bit positions match [EmulatorInput] companion constants.
     *
     * @param port    1 or 2.
     * @param buttons Bitmask of pressed buttons.
     */
    fun setInputState(port: Int, buttons: Int) = nativeSetInputState(port, buttons)

    /**
     * Read back the current button bitmask for one controller port — the value
     * the core will see on its next [Platform::input] poll. Test/diagnostic
     * seam; returns 0 when no core is initialized or the port is unknown.
     */
    fun getInputState(port: Int): Int = nativeGetInputState(port)

    /** Comma-joined names of the buttons held on [port], hardware or software. */
    fun getPressedButtons(port: Int): String = nativeGetPressedButtons(port)

    /**
     * Merge a per-port controller remap. Each `emulated[i]` core button (named
     * as [getPortsJson] reports) is set to read the positional slot named by
     * `source[i]`; both are validated against the staged system. Positional bits
     * are system-independent, so a remap persists across boots and system
     * changes. Passing empty arrays resets the port to defaults. Thread-safe;
     * applied on the render thread's next tick.
     *
     * @return "" on success, or a category-A error string:
     *   "SYSTEM_NOT_LOADED", "INVALID_PARAMETERS", or "UNKNOWN_BUTTON:<name>".
     */
    fun setInputMapping(port: Int, emulated: Array<String>, source: Array<String>): String =
        nativeSetInputMapping(port, emulated, source)

    /**
     * The positional bit a core button currently reads after any remap, or -1
     * if the staged system has no such button. Test/diagnostic seam; call
     * [tick] after [setInputMapping] so the pending remap is applied first.
     */
    fun getButtonBit(port: Int, name: String): Int = nativeGetButtonBit(port, name)

    /**
     * Register (or swap) the device on a port — controllers are explicit, never
     * auto-allocated. An empty [device] disconnects the port. Validated against
     * the staged system; the allocate/rebuild happens on the render thread's
     * next tick, and the registration persists across [loadRom].
     *
     * @return "" on success, or "SYSTEM_NOT_LOADED" / "INVALID_PARAMETERS" /
     *   "UNSUPPORTED_DEVICE".
     */
    fun connectDevice(systemId: String, port: Int, device: String): String =
        nativeConnectDevice(systemId, port, device)

    /** The logical port(s) a physical port's device occupies — 4 for a multitap, else 1. */
    fun devicePorts(systemId: String, port: Int): IntArray = nativeDevicePorts(systemId, port)

    /**
     * Set or clear one software button on a port, resolved against the connected
     * device's own button set (software bits merge with the hardware pad).
     *
     * @return "" on success, or "SYSTEM_NOT_LOADED" / "UNKNOWN_BUTTON:<name>".
     */
    fun pressButton(port: Int, name: String, down: Boolean): String =
        nativePressButton(port, name, down)

    /**
     * Accumulate a relative delta on one axis of the connected device (mouse /
     * light-gun X/Y); consumed on the next poll.
     *
     * @return "" on success, or "SYSTEM_NOT_LOADED" / "INVALID_PARAMETERS".
     */
    fun setAxis(port: Int, name: String, value: Int): String = nativeSetAxis(port, name, value)

    /**
     * Aim a light-gun at an absolute normalized position (0..1). ares' guns are
     * relative-only; native tracks a shadow cursor and feeds the delta to reach
     * the target. Requires the connected device to expose X/Y axes.
     */
    fun aimAt(port: Int, x: Float, y: Float): String = nativeAimAt(port, x, y)

    /** Test seam: pending (unconsumed) accumulated delta on an axis; drained by a poll. */
    fun getAxisAccum(port: Int, name: String): Int = nativeGetAxisAccum(port, name)

    /** Pause emulation — tick() becomes a no-op until resume(). Safe to call from any thread. */
    fun pause() = nativePause()

    /** Resume emulation after a pause(). Safe to call from any thread. */
    fun resume() = nativeResume()

    // -------------------------------------------------------------------------
    // State save / load — GL thread only (ares serializer uses libco)
    // -------------------------------------------------------------------------

    /** Serialize full emulator state to [path]. Returns true on success. */
    fun stateSave(path: String): Boolean = nativeStateSave(path)

    /** Restore emulator state from [path]. Returns true on success. */
    fun stateLoad(path: String): Boolean = nativeStateLoad(path)

    // -------------------------------------------------------------------------
    // Work-RAM access — GL thread only (avoids data race with tick)
    // -------------------------------------------------------------------------

    /**
     * Read [length] bytes of work RAM at bus address [address]. The valid
     * window is system-specific (SFC 0x7E0000–0x7FFFFF, FC 0x0000–0x07FF,
     * GB 0xC000–0xDFFF, MD 0xFF0000–0xFFFFFF).
     * Returns null if the address is out of range or the emulator is not running.
     */
    fun readMemory(address: Int, length: Int): ByteArray? = nativeReadMemory(address, length)

    /**
     * Write [bytes] into work RAM at bus address [address] (see [readMemory]
     * for the per-system windows). Out-of-range writes are silently ignored.
     */
    fun writeMemory(address: Int, bytes: ByteArray) = nativeWriteMemory(address, bytes)

    // -------------------------------------------------------------------------
    // Rewind / run-ahead (GL thread only — state is read inside tick())
    // -------------------------------------------------------------------------

    /**
     * Enable/disable rewind capture. [bufferSeconds] sizes the history
     * (6 snapshots per second); <= 0 keeps ares' desktop default (~16.7 s).
     * Disabling drops the captured history.
     */
    fun configureRewind(enabled: Boolean, bufferSeconds: Int = 0) =
        nativeConfigureRewind(enabled, bufferSeconds)

    /**
     * Enter/exit rewind playback (5× the capture rate, ares desktop
     * semantics; play resumes when history runs out).
     * Returns 1 rewinding, 0 playing, -1 rewind capture not enabled.
     */
    fun toggleRewind(): Int = nativeToggleRewind()

    /**
     * Enable/disable one-frame run-ahead: each tick runs a hidden frame plus
     * a rolled-back visible preview frame, cutting perceived input latency by
     * one frame at 2× emulation cost. Suppressed during fast-forward/rewind.
     */
    fun setRunAhead(enabled: Boolean) = nativeSetRunAhead(enabled)

    /** Mirror the fast-forward flag so run-ahead can suppress itself. Any thread. */
    fun setFastForward(active: Boolean) = nativeSetFastForward(active)

    /**
     * Enable/disable dynamic rate control (default on): each tick skews the
     * stream resamplers by up to ±0.5% toward a half-full audio ring, so
     * production tracks the device DAC clock instead of drifting into
     * overflow or underrun. Any thread.
     */
    fun setDynamicRateControl(enabled: Boolean) = nativeSetDynamicRateControl(enabled)

    /** Gate rumble forwarding from ares' motor nodes. Disabling zeroes the state. */
    fun setRumbleEnabled(enabled: Boolean) = nativeSetRumbleEnabled(enabled)

    /** Current motor state, packed strong shl 16 or weak (u16 each; 0 = off). */
    fun rumbleState(): Int = nativeGetRumbleState()

    // -------------------------------------------------------------------------
    // Cheats (GL thread only — the cheat map is read inside tick())
    // -------------------------------------------------------------------------

    /**
     * Register (or replace) a cheat in ares' raw format: hex "ADDR:VALUE" pairs
     * joined with '+' (e.g. "7E0010:01+7E0011:FF"). Applied on every CPU read
     * of the address from the next tick. Returns false if no valid pair parses.
     */
    fun addCheat(code: String): Boolean = nativeAddCheat(code)

    /** Remove a cheat by its exact code string. Returns false if it wasn't active. */
    fun removeCheat(code: String): Boolean = nativeRemoveCheat(code)

    fun clearCheats() = nativeClearCheats()

    /** Region of the loaded ROM (e.g. "NTSC", "PAL"). Empty if no ROM is loaded. */
    fun getRegion(): String = nativeGetRegion()

    /**
     * JSON array of controller ports and their available buttons.
     * Example: `[{"port":1,"buttons":["B","Y","Select","Start","Up","Down","Left","Right","A","X","L","R"]}]`
     * Returns "[]" if called before [loadSystem].
     */
    fun getPortsJson(): String = nativeGetPortsJson()

    private external fun nativeInit(): Boolean
    private external fun nativeDestroy()
    private external fun nativeVersion(): String

    private external fun nativeSurfaceCreated(surface: Surface)
    private external fun nativeSurfaceChanged(width: Int, height: Int)
    private external fun nativeSurfaceDestroyed()
    private external fun nativeReleaseRenderer()
    private external fun nativePresentFrame(outX: Int, outY: Int, outW: Int, outH: Int): Boolean
    private external fun nativeSetShader(path: String?): Boolean
    private external fun nativeScreenshotRGBA(dims: IntArray): ByteArray?

    private external fun nativeLoadSystem(systemId: String, biosPath: String, backend: String): Boolean
    private external fun nativeGetSupportedSystems(): String
    private external fun nativeGetSystemExtensions(systemId: String): String
    private external fun nativeGetBackendName(): String
    private external fun nativeGetBackendsJson(): String
    private external fun nativeVideoSettingsSupported(): Boolean
    private external fun nativeToggleSupported(key: String): Boolean
    private external fun nativeSetEngineOption(key: String, value: String, staged: Boolean): String
    private external fun nativeGetEngineOptionsJson(): String

    private external fun nativeLoadRom(
        romBytes: ByteArray, savePrefix: String?,
        region: String, preferredRegions: String,
    ): Int
    private external fun nativeStageSlot(index: Int, rom: ByteArray)
    private external fun nativeIsSlotConnected(index: Int): Boolean
    private external fun nativeStageBootOption(name: String, value: String)
    private external fun nativeGetBootOption(name: String): String
    private external fun nativeFlushSaves(): Boolean
    private external fun nativeSetAudio(volume: Float, balance: Float)
    private external fun nativeSetVideo(
        luminance: Float, saturation: Float, gamma: Float,
        colorBleed: Boolean, overscan: Boolean,
    )
    private external fun nativeSetCoreBoolean(key: String, value: Boolean)
    private external fun nativeGetCoreBoolean(key: String): Int

    private external fun nativeTick()
    private external fun nativeGetFrameWidth(): Int
    private external fun nativeGetFrameHeight(): Int
    private external fun nativeGetVideoGeometry(): DoubleArray
    private external fun nativeGetRefreshRateHint(): Double

    private external fun nativeReadAudio(buffer: FloatArray): Int

    private external fun nativeSetInputState(port: Int, buttons: Int)
    private external fun nativeGetInputState(port: Int): Int

    private external fun nativeGetPressedButtons(port: Int): String
    private external fun nativeSetInputMapping(
        port: Int, emulated: Array<String>, source: Array<String>,
    ): String
    private external fun nativeGetButtonBit(port: Int, name: String): Int
    private external fun nativeConnectDevice(systemId: String, port: Int, device: String): String
    private external fun nativeDevicePorts(systemId: String, port: Int): IntArray
    private external fun nativePressButton(port: Int, name: String, down: Boolean): String
    private external fun nativeSetAxis(port: Int, name: String, value: Int): String
    private external fun nativeAimAt(port: Int, x: Float, y: Float): String
    private external fun nativeGetAxisAccum(port: Int, name: String): Int

    private external fun nativePause()
    private external fun nativeResume()

    private external fun nativeStateSave(path: String): Boolean
    private external fun nativeStateLoad(path: String): Boolean

    private external fun nativeReadMemory(address: Int, length: Int): ByteArray?
    private external fun nativeWriteMemory(address: Int, bytes: ByteArray)

    private external fun nativeAddCheat(code: String): Boolean
    private external fun nativeRemoveCheat(code: String): Boolean
    private external fun nativeClearCheats()

    private external fun nativeConfigureRewind(enabled: Boolean, bufferSeconds: Int)
    private external fun nativeToggleRewind(): Int
    private external fun nativeSetRunAhead(enabled: Boolean)
    private external fun nativeSetFastForward(active: Boolean)
    private external fun nativeSetDynamicRateControl(enabled: Boolean)

    private external fun nativeSetRumbleEnabled(enabled: Boolean)
    private external fun nativeGetRumbleState(): Int

    private external fun nativeGetRegion(): String
    private external fun nativeGetPortsJson(): String
}
