package com.kevinbatdorf.plugins.retroemulator

/**
 * Callbacks fired by [EmulatorRenderer] on the render thread for significant lifecycle changes.
 *
 * Implementations are responsible for marshalling to the main thread if they
 * need to dispatch NativePHP events (see [EmulatorFunctions]).
 */
interface EmulatorEventListener {
    fun onStarted(system: String, romPath: String)
    fun onStopped()
    fun onPaused()
    fun onResumed()

    /** Default no-op — the renderer can't reference EmulatorFunctions (the dev harness excludes it). */
    fun onWindowMetrics() {}

    /**
     * Fired when a watched address changes value.
     *
     * @param address  Console bus address in the system's work-RAM window.
     * @param oldValue Previous packed integer value.
     * @param newValue New packed integer value.
     */
    fun onMemoryChanged(address: Int, oldValue: Int, newValue: Int)

    /**
     * Fired when [EmulatorRenderer.syncReadMemory] is used asynchronously
     * (i.e., via the [EmulatorFunctions.ReadMemoryAsync] bridge).
     */
    fun onMemoryRead(address: Int, bytes: ByteArray)

    /** Fired on runtime errors (emulator crash, audio device lost, etc.). */
    fun onError(code: String, message: String)
}
