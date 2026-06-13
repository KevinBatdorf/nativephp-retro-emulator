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
     * Initialise the SFC (SNES) system node tree. Must be called from the GL
     * thread because it primes the libco scheduler context for this thread.
     *
     * @param iplRomBytes  64-byte SPC700 boot ROM from the user's hardware.
     *                     Pass null to use a zero-filled stub (SMP will hang
     *                     at audio init but the emulator will not crash).
     * @param boardsBmlBytes  Content of Super Famicom Boards.bml from the mia
     *                        database — loaded from R.raw.super_famicom_boards.
     */
    fun loadSystem(iplRomBytes: ByteArray?, boardsBmlBytes: ByteArray): Boolean =
        nativeLoadSystem(iplRomBytes, boardsBmlBytes)

    /**
     * Load a ROM image and power on the emulator. Must be called after
     * [loadSystem] and from the GL thread.
     *
     * @param romBytes Raw ROM bytes. A 512-byte copier header is stripped
     *                 automatically if detected.
     */
    fun loadRom(romBytes: ByteArray): Boolean = nativeLoadRom(romBytes)

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
    // Native declarations
    // -------------------------------------------------------------------------

    private external fun nativeInit(): Boolean
    private external fun nativeDestroy()
    private external fun nativeVersion(): String

    private external fun nativeSetupRenderer(textureId: Int)

    private external fun nativeLoadSystem(
        iplRomBytes: ByteArray?,
        boardsBmlBytes: ByteArray
    ): Boolean

    private external fun nativeLoadRom(romBytes: ByteArray): Boolean

    private external fun nativeTick()
    private external fun nativeGetFrameWidth(): Int
    private external fun nativeGetFrameHeight(): Int

    private external fun nativeReadAudio(buffer: FloatArray): Int

    private external fun nativeSetInputState(port: Int, buttons: Int)
}
