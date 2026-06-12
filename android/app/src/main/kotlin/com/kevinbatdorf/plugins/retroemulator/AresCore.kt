package com.kevinbatdorf.plugins.retroemulator

/**
 * JNI wrapper around the ares emulator core.
 *
 * Lifecycle: call [init] once, then [tick] each frame, then [destroy] on teardown.
 * All methods must be called from the same thread.
 */
class AresCore {

    init {
        System.loadLibrary("retro_emulator")
    }

    /** Initialise the ares platform singleton. Returns false if already initialised. */
    fun init(): Boolean = nativeInit()

    /** Tick the emulator one frame. No-op until a system is loaded (Phase 4+). */
    fun tick() = nativeTick()

    /** Tear down the ares platform singleton. */
    fun destroy() = nativeDestroy()

    /** Returns the ares version string (e.g. "v140-android"). */
    fun version(): String = nativeVersion()

    private external fun nativeInit(): Boolean
    private external fun nativeTick()
    private external fun nativeDestroy()
    private external fun nativeVersion(): String
}
