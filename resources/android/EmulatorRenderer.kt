package com.kevinbatdorf.plugins.retroemulator

import android.content.Context
import android.graphics.Bitmap
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

private const val TAG = "EmulatorRenderer"

// Texture dimensions — large enough for any SFC canvas (max ~564×448).
private const val TEX_W = 1024
private const val TEX_H = 512

// Emulation pacing. 60.0988 is NTSC SNES/NES; GB (59.73) and MD (59.92) are
// close enough that the audio resampler absorbs the drift. Refined per-system
// (and for PAL) in the feature-audit phase.
private const val TARGET_FPS = 60.0988
private const val MAX_TICKS_PER_FRAME = 8

// Battery-save auto-flush cadence — matches ares' desktop 30 s auto-save.
private const val AUTOSAVE_INTERVAL_NANOS = 30_000_000_000L

/**
 * A [GLSurfaceView] that drives the ares emulator and blits each frame via a
 * full-screen textured quad. All ares calls happen on the GL thread so libco
 * coroutine contexts are consistent throughout the session.
 *
 * Usage:
 * 1. Construct and set as the Activity's content view.
 * 2. Call [pendingSystemLoad] / [pendingRomLoad] with your data; the actual
 *    native calls execute on the first [onDrawFrame] after each flag is set.
 */
class EmulatorRenderer(context: Context) : GLSurfaceView(context) {

    private val core  = AresCore()
    private val audio = EmulatorAudio(core)
    val input = EmulatorInput(core)

    init {
        // Hardware gamepads deliver key/motion events to the focused view.
        // In an EDGE host nothing else routes them here (the plugin's old
        // test activity overrode dispatchKeyEvent; hosts don't).
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean =
        input.onKeyEvent(event) || super.onKeyDown(keyCode, event)

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean =
        input.onKeyEvent(event) || super.onKeyUp(keyCode, event)

    override fun onGenericMotionEvent(event: MotionEvent): Boolean =
        input.onMotionEvent(event) || super.onGenericMotionEvent(event)

    override fun onWindowFocusChanged(hasWindowFocus: Boolean) {
        super.onWindowFocusChanged(hasWindowFocus)
        // Buttons held across a focus loss would otherwise stay pressed forever.
        if (!hasWindowFocus) input.reset()
    }

    // Pending commands posted from the main thread and consumed on the GL thread.
    @Volatile var pendingSystemId: String?    = null
    @Volatile var pendingRomBytes: ByteArray? = null
    @Volatile var pendingSavePrefix: String?  = null

    @Volatile private var systemLoaded = false
    @Volatile private var romLoaded    = false
    @Volatile private var audioStarted = false
    @Volatile private var firstFrameSent = false

    // ROM metadata — set alongside romLoaded = true.
    @Volatile var loadedSystem: String = ""
    @Volatile var loadedRomPath: String = ""

    /** Current logical emulator status, updated on the GL thread. */
    @Volatile var currentStatus: String = "stopped"
        private set

    /** Optional listener for emulator lifecycle events — dispatched on the GL thread. */
    var eventListener: EmulatorEventListener? = null

    // Phase 7 — synchronous GL-thread operations.
    // Requests are posted from bridge threads and serviced on the next onDrawFrame.

    private data class MemReadRequest(
        val address: Int,
        val length: Int,
        val latch: CountDownLatch,
        var result: ByteArray? = null,
    )
    private val pendingRead = AtomicReference<MemReadRequest?>(null)

    private class PortsReadRequest(val latch: CountDownLatch) {
        @Volatile var result: String? = null
    }

    private val pendingPortsRead = AtomicReference<PortsReadRequest?>(null)

    private data class MemWriteRequest(val address: Int, val bytes: ByteArray)
    private val pendingWrite = AtomicReference<MemWriteRequest?>(null)

    private data class StateOpRequest(
        val path: String,
        val save: Boolean,
        val latch: CountDownLatch,
        var result: Boolean = false,
    )
    private val pendingStateOp = AtomicReference<StateOpRequest?>(null)

    private data class ScreenshotRequest(
        val latch: CountDownLatch,
        var result: ByteArray? = null,
    )
    private val pendingScreenshot = AtomicReference<ScreenshotRequest?>(null)

    // Phase 7 — memory watches.
    // Keyed by bus address; value is the last known raw byte(s) as an Int
    // (single-byte watches use bits 0–7; multi-byte watches use a packed value).
    private data class WatchEntry(val address: Int, val length: Int, var lastValue: Int)
    private val watchedAddresses = ConcurrentHashMap<Int, WatchEntry>()

    // Phase 7/14 — fast-forward and speed multiplier. fastForward takes
    // precedence (4×); otherwise speedMultiplier scales the tick budget.
    @Volatile var fastForward: Boolean = false
    @Volatile var speedMultiplier: Double = 1.0

    // Frame pacing — the GL thread draws at the display's refresh rate (120 Hz
    // on some devices) and GLSurfaceView redraws opportunistically, so ticks
    // are budgeted by wall clock against the console's native frame rate.
    private var lastTickNanos = 0L
    private var tickAccumulator = 0.0

    // Phase 13 — periodic battery-save flush (30 s, matching ares' desktop
    // auto-save cadence). Set false via LoadSystem config { autoSave: false }.
    @Volatile var autoSave: Boolean = true
    private var lastAutoSaveNanos = 0L

    // Current UV extents for the content region within the 1024×512 texture.
    private var uvW = 1f
    private var uvH = 1f

    private val renderer = object : GLSurfaceView.Renderer {

        private var textureId  = 0
        private var program    = 0
        private var posLoc     = 0
        private var texLoc     = 0
        private var uvScaleLoc = 0
        private var vbo        = 0

        override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
            GLES20.glClearColor(0f, 0f, 0f, 1f)

            // Compile shaders.
            program    = buildProgram(VERTEX_SRC, FRAGMENT_SRC)
            posLoc     = GLES20.glGetAttribLocation(program,  "aPos")
            texLoc     = GLES20.glGetUniformLocation(program, "uTex")
            uvScaleLoc = GLES20.glGetUniformLocation(program, "uUvScale")

            // Full-screen quad (two triangles as a triangle strip, NDC coords).
            val verts = floatArrayOf(
                -1f, -1f,
                 1f, -1f,
                -1f,  1f,
                 1f,  1f
            )
            val buf: FloatBuffer = ByteBuffer
                .allocateDirect(verts.size * 4)
                .order(ByteOrder.nativeOrder())
                .asFloatBuffer()
                .put(verts)
                .also { it.position(0) }

            val vbos = IntArray(1)
            GLES20.glGenBuffers(1, vbos, 0)
            vbo = vbos[0]
            GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, vbo)
            GLES20.glBufferData(
                GLES20.GL_ARRAY_BUFFER,
                verts.size * 4,
                buf,
                GLES20.GL_STATIC_DRAW
            )

            // Allocate the frame texture.
            val texIds = IntArray(1)
            GLES20.glGenTextures(1, texIds, 0)
            textureId = texIds[0]
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S,     GLES20.GL_CLAMP_TO_EDGE)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T,     GLES20.GL_CLAMP_TO_EDGE)
            // Allocate storage (zeroed = black).
            GLES20.glTexImage2D(
                GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA,
                TEX_W, TEX_H, 0,
                GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null
            )

            // Initialise ares and hand it the texture ID.
            core.init()
            core.setupRenderer(textureId)
            Log.i(TAG, "Surface created — ares ${core.version()}")
        }

        override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
            GLES20.glViewport(0, 0, width, height)
        }

        override fun onDrawFrame(gl: GL10?) {
            // --- Consume pending system load ---
            val systemId = pendingSystemId
            if (!systemLoaded && systemId != null) {
                systemLoaded = core.loadSystem(systemId)
                if (!systemLoaded) {
                    // Consume the request — retrying an id ares rejected
                    // once would fail identically every frame, forever.
                    pendingSystemId = null
                    Log.e(TAG, "loadSystem($systemId) failed")
                }
            }

            // --- Service pending ports read here so it orders after a queued system load ---
            pendingPortsRead.getAndSet(null)?.let { req ->
                req.result = if (systemLoaded) core.getPortsJson() else null
                req.latch.countDown()
            }

            // --- Consume pending ROM load ---
            val rom = pendingRomBytes
            if (systemLoaded && !romLoaded && rom != null) {
                romLoaded = core.loadRom(rom, pendingSavePrefix)
                pendingRomBytes = null
                if (romLoaded) {
                    currentStatus = "loading"
                } else {
                    Log.e(TAG, "loadRom failed")
                }
            }

            // --- Start audio once the ROM is running ---
            if (romLoaded && !audioStarted) {
                audioStarted = true
                audio.start()
            }

            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)

            if (!romLoaded) return

            // --- Service pending memory write (before tick so write takes effect this frame) ---
            pendingWrite.getAndSet(null)?.let { req ->
                core.writeMemory(req.address, req.bytes)
            }

            // --- Tick the emulator, paced to the console's frame rate ---
            val now = System.nanoTime()
            if (lastTickNanos == 0L) lastTickNanos = now
            // Clamp long gaps (pause/resume, app switch) so we don't fast-run.
            val elapsed = ((now - lastTickNanos) / 1e9).coerceAtMost(0.25)
            lastTickNanos = now

            val speed = if (fastForward) 4.0 else speedMultiplier
            tickAccumulator += elapsed * TARGET_FPS * speed
            var ticks = tickAccumulator.toInt()
            if (ticks > MAX_TICKS_PER_FRAME) {
                // Can't keep up — drop the debt instead of spiraling.
                ticks = MAX_TICKS_PER_FRAME
                tickAccumulator = 0.0
            } else {
                tickAccumulator -= ticks
            }
            repeat(ticks) { core.tick() }

            // --- Periodic battery-save flush ---
            if (autoSave) {
                if (lastAutoSaveNanos == 0L) lastAutoSaveNanos = now
                if (now - lastAutoSaveNanos >= AUTOSAVE_INTERVAL_NANOS) {
                    lastAutoSaveNanos = now
                    core.flushSaves()
                }
            }

            // --- Fire EmulatorStarted on the first rendered frame ---
            val fw = core.getFrameWidth()
            val fh = core.getFrameHeight()
            if (!firstFrameSent && fw > 0 && fh > 0) {
                firstFrameSent = true
                currentStatus = "running"
                eventListener?.onStarted(loadedSystem, loadedRomPath)
            }

            // --- Update UV scale ---
            if (fw > 0 && fh > 0) {
                uvW = fw.toFloat() / TEX_W.toFloat()
                uvH = fh.toFloat() / TEX_H.toFloat()
            }

            // --- Service pending memory read ---
            pendingRead.getAndSet(null)?.let { req ->
                req.result = core.readMemory(req.address, req.length)
                req.latch.countDown()
            }

            // --- Service pending state operation ---
            pendingStateOp.getAndSet(null)?.let { req ->
                req.result = if (req.save) core.stateSave(req.path) else core.stateLoad(req.path)
                req.latch.countDown()
            }

            // --- Service pending screenshot ---
            pendingScreenshot.getAndSet(null)?.let { req ->
                req.result = captureFramebuffer(fw, fh)
                req.latch.countDown()
            }

            // --- Check memory watches and fire MemoryChanged events ---
            if (watchedAddresses.isNotEmpty()) {
                for (entry in watchedAddresses.values) {
                    val bytes = core.readMemory(entry.address, entry.length) ?: continue
                    val newValue = packBytesToInt(bytes)
                    if (newValue != entry.lastValue) {
                        val oldValue = entry.lastValue
                        entry.lastValue = newValue
                        eventListener?.onMemoryChanged(entry.address, oldValue, newValue)
                    }
                }
            }

            // Draw the full-screen quad.
            GLES20.glUseProgram(program)

            GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, vbo)
            GLES20.glEnableVertexAttribArray(posLoc)
            GLES20.glVertexAttribPointer(posLoc, 2, GLES20.GL_FLOAT, false, 8, 0)

            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId)
            GLES20.glUniform1i(texLoc, 0)
            GLES20.glUniform2f(uvScaleLoc, uvW, uvH)

            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)

            GLES20.glDisableVertexAttribArray(posLoc)
        }
    }

    init {
        setEGLContextClientVersion(2)
        setRenderer(renderer)
        renderMode = RENDERMODE_CONTINUOUSLY
        // Gamepad input doesn't reset Android's idle timer — keep the display
        // awake while the emulator view is showing.
        keepScreenOn = true
    }

    // ---------------------------------------------------------------------------
    // Public API — called from any thread; GL-critical work is queued/synced.
    // ---------------------------------------------------------------------------

    /**
     * Queue a system load; it executes on the next [onDrawFrame].
     * @param systemId ares system ID — one of [AresCore.supportedSystems].
     */
    fun queueSystemLoad(systemId: String) {
        pendingSystemId = systemId
        requestRender()
    }

    /**
     * Queue a ROM load; it executes on the next [onDrawFrame] after the system is ready.
     * @param system     ares system ID (e.g. "sfc") — stored for [EmulatorStarted] event.
     * @param romPath    Absolute file path — stored for [EmulatorStarted] event.
     * @param savePrefix Battery-save file prefix (see [AresCore.loadRom]); null
     *                   disables persistence.
     */
    fun queueRomLoad(
        romBytes: ByteArray,
        system: String = "sfc",
        romPath: String = "",
        savePrefix: String? = null,
    ) {
        // Cheat addresses are game knowledge — stale codes applied to a new ROM
        // would corrupt it unpredictably. Queued before pendingRomBytes so any
        // addCheat issued after this call survives the swap.
        queueEvent { core.clearCheats() }
        loadedSystem  = system
        loadedRomPath = romPath
        firstFrameSent = false
        pendingSavePrefix = savePrefix
        pendingRomBytes = romBytes
        requestRender()
    }

    /** Pause emulation. Audio keeps running; tick() becomes a no-op until [resumeEmulation]. */
    fun pauseEmulation() {
        core.pause()
        queueEvent { core.flushSaves() }
        currentStatus = "paused"
        eventListener?.onPaused()
    }

    /** Resume emulation after [pauseEmulation]. */
    fun resumeEmulation() {
        core.resume()
        currentStatus = if (firstFrameSent) "running" else "loading"
        eventListener?.onResumed()
    }

    /**
     * Stop emulation and tear down the ares core.
     * Audio is stopped on the calling thread; core teardown runs on the GL thread.
     */
    fun stopEmulation() {
        watchedAddresses.clear()
        input.reset()
        audio.stop()
        queueEvent {
            core.flushSaves()
            core.destroy()
            // Restore the surface-created invariant (core initialized) so a
            // follow-up loadSystem works — system switching goes through here.
            core.init()
            romLoaded     = false
            systemLoaded  = false
            audioStarted  = false
            firstFrameSent = false
            currentStatus = "stopped"
        }
        eventListener?.onStopped()
    }

    /**
     * Stop audio and tear down the ares core. Call from [Activity.onDestroy].
     */
    fun release() {
        watchedAddresses.clear()
        input.reset()
        audio.stop()
        queueEvent {
            core.flushSaves()
            core.destroy()
        }
    }

    // ---------------------------------------------------------------------------
    // Phase 7 — synchronous GL-thread read/write helpers
    // ---------------------------------------------------------------------------

    /**
     * Read [length] bytes from WRAM at bus [address] (0x7E0000–0x7FFFFF).
     * Blocks the calling thread until the GL thread services the request (≤2 s).
     * Returns null if the address is invalid or the emulator is not running.
     */
    fun syncReadMemory(address: Int, length: Int): ByteArray? {
        val latch = CountDownLatch(1)
        val req   = MemReadRequest(address, length, latch)
        pendingRead.set(req)
        requestRender()
        latch.await(2, TimeUnit.SECONDS)
        return req.result
    }

    /**
     * Write [bytes] to WRAM at bus [address]. The write is applied before the next tick.
     */
    fun queueWriteMemory(address: Int, bytes: ByteArray) {
        pendingWrite.set(MemWriteRequest(address, bytes))
        requestRender()
    }

    /**
     * Save emulator state to [path]. Blocks until the GL thread completes (≤30 s).
     * Returns true on success.
     */
    fun syncStateSave(path: String): Boolean {
        val latch = CountDownLatch(1)
        val req   = StateOpRequest(path, save = true, latch)
        pendingStateOp.set(req)
        requestRender()
        latch.await(30, TimeUnit.SECONDS)
        return req.result
    }

    /**
     * Load emulator state from [path]. Blocks until the GL thread completes (≤30 s).
     * Returns true on success.
     */
    fun syncStateLoad(path: String): Boolean {
        val latch = CountDownLatch(1)
        val req   = StateOpRequest(path, save = false, latch)
        pendingStateOp.set(req)
        requestRender()
        latch.await(30, TimeUnit.SECONDS)
        return req.result
    }

    /**
     * Capture the current framebuffer as a PNG-encoded [ByteArray].
     * Blocks until the GL thread completes (≤5 s). Returns null on failure.
     */
    fun syncScreenshot(): ByteArray? {
        val latch = CountDownLatch(1)
        val req   = ScreenshotRequest(latch)
        pendingScreenshot.set(req)
        requestRender()
        latch.await(5, TimeUnit.SECONDS)
        return req.result
    }

    // ---------------------------------------------------------------------------
    // Cheats — mutations run on the GL thread (the cheat map is read inside tick)
    // ---------------------------------------------------------------------------

    /**
     * Register (or replace) a cheat code. Blocks until the GL thread parses it
     * (≤2 s). Returns false if no valid ADDR:VALUE pair parsed, null on timeout.
     */
    fun syncAddCheat(code: String): Boolean? = syncOnGlThread { core.addCheat(code) }

    /**
     * Remove a cheat by exact code string. Blocks ≤2 s. Returns false if the
     * code wasn't active, null on timeout.
     */
    fun syncRemoveCheat(code: String): Boolean? = syncOnGlThread { core.removeCheat(code) }

    /** Deactivate all cheats. Fire-and-forget. */
    fun queueClearCheats() = queueEvent { core.clearCheats() }

    /** Run [block] on the GL thread and block the caller for the result (≤2 s). */
    private fun <T> syncOnGlThread(block: () -> T): T? {
        val latch = CountDownLatch(1)
        var result: T? = null
        queueEvent {
            result = block()
            latch.countDown()
        }
        requestRender()
        latch.await(2, TimeUnit.SECONDS)
        return result
    }

    // ---------------------------------------------------------------------------
    // Phase 7 — memory watches
    // ---------------------------------------------------------------------------

    /**
     * Add or merge address watches. Each entry is either a plain bus address (Int)
     * or a map with "address" and "length" keys. Watches are preserved across ROM swaps
     * — call [clearMemoryWatches] explicitly if needed.
     *
     * @param entries List of addresses: each element is Int or Map<String,Int>.
     */
    fun addWatches(entries: List<Any>) {
        for (entry in entries) {
            when (entry) {
                is Int -> {
                    watchedAddresses.putIfAbsent(entry, WatchEntry(entry, 1, -1))
                }
                is Map<*, *> -> {
                    val addr = (entry["address"] as? Number)?.toInt() ?: continue
                    val len  = (entry["length"]  as? Number)?.toInt() ?: 1
                    watchedAddresses[addr] = WatchEntry(addr, len, -1)
                }
            }
        }
    }

    /** Remove watches for the given bus addresses. */
    fun removeWatches(addresses: List<Int>) {
        addresses.forEach { watchedAddresses.remove(it) }
    }

    /** Clear all memory watches. */
    fun clearMemoryWatches() = watchedAddresses.clear()

    // ---------------------------------------------------------------------------
    // Phase 7 — GL-thread helpers (called from onDrawFrame)
    // ---------------------------------------------------------------------------

    private fun captureFramebuffer(fw: Int, fh: Int): ByteArray? {
        if (fw <= 0 || fh <= 0) return null
        return try {
            val buf = ByteBuffer.allocateDirect(fw * fh * 4).order(ByteOrder.nativeOrder())
            GLES20.glReadPixels(0, 0, fw, fh, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, buf)
            // GL origin is bottom-left; flip vertically for correct top-down orientation.
            val src = ByteArray(fw * fh * 4).also { buf.get(it) }
            val flipped = ByteArray(src.size)
            val rowBytes = fw * 4
            for (row in 0 until fh) {
                src.copyInto(flipped, row * rowBytes, (fh - 1 - row) * rowBytes, (fh - row) * rowBytes)
            }
            val bmp = Bitmap.createBitmap(fw, fh, Bitmap.Config.ARGB_8888)
            bmp.copyPixelsFromBuffer(ByteBuffer.wrap(flipped))
            val bos = ByteArrayOutputStream()
            bmp.compress(Bitmap.CompressFormat.PNG, 90, bos)
            bmp.recycle()
            bos.toByteArray()
        } catch (e: Exception) {
            Log.e(TAG, "Screenshot failed: ${e.message}")
            null
        }
    }

    // ---------------------------------------------------------------------------
    // Phase 14 — audio / video options
    // ---------------------------------------------------------------------------

    /** Master volume (0–1) and stereo balance (−1 … +1). Safe from any thread. */
    fun setAudioOptions(volume: Float, balance: Float) = core.setAudio(volume, balance)

    /** Queue video post-processing options onto the GL thread. */
    fun queueVideoOptions(
        luminance: Float,
        saturation: Float,
        gamma: Float,
        colorBleed: Boolean,
        interframeBlending: Boolean,
    ) {
        queueEvent { core.setVideo(luminance, saturation, gamma, colorBleed, interframeBlending) }
    }

    /**
     * Region of the loaded ROM (e.g. "NTSC", "PAL"). Safe to call from any thread
     * once the ROM is loaded — the native value is written before [romLoaded] is set.
     */
    fun getRegion(): String = core.getRegion()

    /**
     * JSON array of controller ports with button names. Safe from any thread after
     * [systemLoaded] is true — built once during system load and read-only afterward.
     */
    fun getPortsJson(): String = core.getPortsJson()

    /**
     * Ports JSON is built by the system load on the GL thread — round-trip
     * through it so a GetPorts issued right after LoadSystem orders correctly.
     * Returns null if no system is loaded within the wait window.
     */
    fun syncGetPortsJson(): String? {
        val latch = CountDownLatch(1)
        val req = PortsReadRequest(latch)
        pendingPortsRead.set(req)
        requestRender()
        latch.await(2, TimeUnit.SECONDS)
        return req.result
    }

    private fun packBytesToInt(bytes: ByteArray): Int {
        var value = 0
        for (i in bytes.indices.reversed()) {
            value = (value shl 8) or (bytes[i].toInt() and 0xFF)
        }
        return value
    }
}

// ---------------------------------------------------------------------------
// GL shader sources
// ---------------------------------------------------------------------------

// The ares video callback writes ARGB8888 (little-endian: BGRA in memory).
// When uploaded as GL_RGBA the channels are in BGRA order from GL's perspective,
// so the fragment shader swizzles .bgra → RGBA to display correctly.
private const val VERTEX_SRC = """
    attribute vec2 aPos;
    uniform   vec2 uUvScale;
    varying   vec2 vUv;
    void main() {
        // Flip Y so row 0 of the ares output (screen top) maps to the top of
        // the quad (GL NDC Y = +1).  Scale into the content region of the texture.
        vUv = vec2(
            (aPos.x * 0.5 + 0.5) * uUvScale.x,
            (0.5 - aPos.y * 0.5) * uUvScale.y
        );
        gl_Position = vec4(aPos, 0.0, 1.0);
    }
"""

private const val FRAGMENT_SRC = """
    precision mediump float;
    uniform sampler2D uTex;
    varying vec2      vUv;
    void main() {
        vec4 c = texture2D(uTex, vUv);
        // Swap R↔B: ares BGRA memory → correct RGBA display.
        gl_FragColor = c.bgra;
    }
"""

// ---------------------------------------------------------------------------
// GL helpers
// ---------------------------------------------------------------------------

private fun compileShader(type: Int, src: String): Int {
    val id = GLES20.glCreateShader(type)
    GLES20.glShaderSource(id, src)
    GLES20.glCompileShader(id)
    val status = IntArray(1)
    GLES20.glGetShaderiv(id, GLES20.GL_COMPILE_STATUS, status, 0)
    if (status[0] == 0) {
        Log.e(TAG, "Shader compile error: ${GLES20.glGetShaderInfoLog(id)}")
        GLES20.glDeleteShader(id)
        return 0
    }
    return id
}

private fun buildProgram(vertSrc: String, fragSrc: String): Int {
    val vert = compileShader(GLES20.GL_VERTEX_SHADER,   vertSrc)
    val frag = compileShader(GLES20.GL_FRAGMENT_SHADER, fragSrc)
    val prog = GLES20.glCreateProgram()
    GLES20.glAttachShader(prog, vert)
    GLES20.glAttachShader(prog, frag)
    GLES20.glLinkProgram(prog)
    val status = IntArray(1)
    GLES20.glGetProgramiv(prog, GLES20.GL_LINK_STATUS, status, 0)
    if (status[0] == 0) {
        Log.e(TAG, "Program link error: ${GLES20.glGetProgramInfoLog(prog)}")
    }
    GLES20.glDeleteShader(vert)
    GLES20.glDeleteShader(frag)
    return prog
}
