package com.kevinbatdorf.plugins.retroemulator

/**
 * JNI wrapper around the ares emulator core.
 *
 * Threading: [init], [loadSystem], [loadRom], [tick], and [destroy] MUST be
 * called from the same thread (the GL thread) because the ares scheduler uses
 * libco coroutines that capture their stack context at [loadSystem] time.
 *
 * Lifecycle:
 *   init() → setupRenderer() → loadSystem() → loadRom() → tick() … → destroy()
 */
class AresCore {

    init {
        System.loadLibrary("retro_emulator")
    }

    // -------------------------------------------------------------------------
    // Phase 3 — core lifecycle
    // -------------------------------------------------------------------------

    /** Initialise the ares platform singleton. Returns false if already initialised. */
    fun init(): Boolean = nativeInit()

    /** Tear down the ares platform singleton and free all emulator state. */
    fun destroy() = nativeDestroy()

    /** Returns the ares version string (e.g. "v140-android"). */
    fun version(): String = nativeVersion()

    // -------------------------------------------------------------------------
    // Phase 4 — rendering
    // -------------------------------------------------------------------------

    /**
     * Bind a GL texture for frame delivery. Must be called from the GL thread
     * after the texture has been allocated with glTexImage2D.
     */
    fun setupRenderer(textureId: Int) = nativeSetupRenderer(textureId)

    // -------------------------------------------------------------------------
    // Phase 4 — system and ROM loading (GL thread only)
    // -------------------------------------------------------------------------

    /**
     * Initialise the system node tree for the given ares system ID. Must be
     * called from the GL thread because it primes the libco scheduler context
     * for this thread.
     *
     * System firmware (SFC ipl.rom + boards.bml, GB boot ROM, MD TMSS) is
     * embedded in the native library — no assets need to be provided.
     *
     * @param systemId One of [supportedSystems] (e.g. "sfc", "fc", "gb", "md").
     */
    fun loadSystem(systemId: String): Boolean = nativeLoadSystem(systemId)

    /** Comma-separated ares system IDs compiled into this build (e.g. "fc,sfc,gb,md"). */
    fun supportedSystems(): String = nativeGetSupportedSystems()

    /**
     * Load a ROM image and power on the emulator. Must be called after
     * [loadSystem] and from the GL thread.
     *
     * @param romBytes   Raw ROM bytes. A 512-byte copier header is stripped
     *                   automatically if detected.
     * @param savePrefix Battery-save location prefix — files are written as
     *                   "<prefix>.save.ram", "<prefix>.save.eeprom", etc.
     *                   Existing files seed the cartridge before boot.
     *                   Null disables persistence.
     */
    fun loadRom(romBytes: ByteArray, savePrefix: String? = null): Boolean =
        nativeLoadRom(romBytes, savePrefix)

    /**
     * Write current battery-backed memory to disk under the prefix passed to
     * [loadRom]. GL thread only. Returns false when nothing was persisted.
     */
    fun flushSaves(): Boolean = nativeFlushSaves()

    // -------------------------------------------------------------------------
    // Phase 14 — audio / video options
    // -------------------------------------------------------------------------

    /** Master volume (0–1) and stereo balance (−1 left … +1 right). Any thread. */
    fun setAudio(volume: Float, balance: Float) = nativeSetAudio(volume, balance)

    /**
     * Video post-processing on the ares screen node. Ranges follow ares:
     * luminance/saturation 0–1, gamma 1.0–2.0. GL thread only.
     */
    fun setVideo(
        luminance: Float,
        saturation: Float,
        gamma: Float,
        colorBleed: Boolean,
        interframeBlending: Boolean,
    ) = nativeSetVideo(luminance, saturation, gamma, colorBleed, interframeBlending)

    // -------------------------------------------------------------------------
    // Phase 4 — per-frame operations (GL thread only)
    // -------------------------------------------------------------------------

    /**
     * Run one emulated frame. The video callback fires synchronously and uploads
     * the frame to the texture bound via [setupRenderer]. No-op if no ROM is
     * loaded.
     */
    fun tick() = nativeTick()

    /** Width of the most recently rendered frame in pixels (0 before first frame). */
    fun getFrameWidth(): Int = nativeGetFrameWidth()

    /** Height of the most recently rendered frame in pixels (0 before first frame). */
    fun getFrameHeight(): Int = nativeGetFrameHeight()

    // -------------------------------------------------------------------------
    // Phase 5 — audio
    // -------------------------------------------------------------------------

    /**
     * Drain mixed stereo audio samples from the native ring buffer into
     * [buffer] (interleaved L/R float pairs, range −1..+1).
     *
     * @return Number of floats written (always even; 0 if nothing pending).
     *         May be called from any thread.
     */
    fun readAudio(buffer: FloatArray): Int = nativeReadAudio(buffer)

    // -------------------------------------------------------------------------
    // Phase 6 — input
    // -------------------------------------------------------------------------

    /**
     * Write the current button bitmask for one controller port. Safe to call
     * from any thread. Bit positions match [EmulatorInput] companion constants.
     *
     * @param port    1 or 2.
     * @param buttons Bitmask of pressed buttons.
     */
    fun setInputState(port: Int, buttons: Int) = nativeSetInputState(port, buttons)

    // -------------------------------------------------------------------------
    // Phase 7 — emulator control (any thread)
    // -------------------------------------------------------------------------

    /** Pause emulation — tick() becomes a no-op until resume(). Safe to call from any thread. */
    fun pause() = nativePause()

    /** Resume emulation after a pause(). Safe to call from any thread. */
    fun resume() = nativeResume()

    // -------------------------------------------------------------------------
    // Phase 7 — state save / load (GL thread only — ares serializer uses libco)
    // -------------------------------------------------------------------------

    /** Serialize full emulator state to [path]. Returns true on success. */
    fun stateSave(path: String): Boolean = nativeStateSave(path)

    /** Restore emulator state from [path]. Returns true on success. */
    fun stateLoad(path: String): Boolean = nativeStateLoad(path)

    // -------------------------------------------------------------------------
    // Phase 7 — work-RAM access (GL thread only — avoids data race with tick)
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
    // Phase 7 — ROM metadata (safe from any thread after loadRom)
    // -------------------------------------------------------------------------

    /** Region of the loaded ROM (e.g. "NTSC", "PAL"). Empty if no ROM is loaded. */
    fun getRegion(): String = nativeGetRegion()

    /**
     * JSON array of controller ports and their available buttons.
     * Example: `[{"port":1,"buttons":["B","Y","Select","Start","Up","Down","Left","Right","A","X","L","R"]}]`
     * Returns "[]" if called before [loadSystem].
     */
    fun getPortsJson(): String = nativeGetPortsJson()

    // -------------------------------------------------------------------------
    // Native declarations
    // -------------------------------------------------------------------------

    private external fun nativeInit(): Boolean
    private external fun nativeDestroy()
    private external fun nativeVersion(): String

    private external fun nativeSetupRenderer(textureId: Int)

    private external fun nativeLoadSystem(systemId: String): Boolean
    private external fun nativeGetSupportedSystems(): String

    private external fun nativeLoadRom(romBytes: ByteArray, savePrefix: String?): Boolean
    private external fun nativeFlushSaves(): Boolean
    private external fun nativeSetAudio(volume: Float, balance: Float)
    private external fun nativeSetVideo(
        luminance: Float, saturation: Float, gamma: Float,
        colorBleed: Boolean, interframeBlending: Boolean,
    )

    private external fun nativeTick()
    private external fun nativeGetFrameWidth(): Int
    private external fun nativeGetFrameHeight(): Int

    private external fun nativeReadAudio(buffer: FloatArray): Int

    private external fun nativeSetInputState(port: Int, buttons: Int)

    private external fun nativePause()
    private external fun nativeResume()

    private external fun nativeStateSave(path: String): Boolean
    private external fun nativeStateLoad(path: String): Boolean

    private external fun nativeReadMemory(address: Int, length: Int): ByteArray?
    private external fun nativeWriteMemory(address: Int, bytes: ByteArray)

    private external fun nativeGetRegion(): String
    private external fun nativeGetPortsJson(): String
}
