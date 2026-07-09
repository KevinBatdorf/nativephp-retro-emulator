package com.kevinbatdorf.plugins.retroemulator

import android.content.Context
import android.graphics.Bitmap
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.util.Log
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

    // Pending commands posted from the main thread and consumed on the GL thread.
    @Volatile var pendingIplRom: ByteArray?    = null
    @Volatile var pendingBoardsBml: ByteArray? = null
    @Volatile var pendingRomBytes: ByteArray?  = null

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

    // Phase 7 — fast-forward / speed.
    @Volatile var fastForward: Boolean = false

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
            val bml = pendingBoardsBml
            if (!systemLoaded && bml != null) {
                val ipl = pendingIplRom
                systemLoaded = core.loadSystem(ipl, bml)
                if (!systemLoaded) Log.e(TAG, "loadSystem failed")
            }

            // --- Consume pending ROM load ---
            val rom = pendingRomBytes
            if (systemLoaded && !romLoaded && rom != null) {
                romLoaded = core.loadRom(rom)
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

            // --- Tick the emulator (fast-forward = 4 ticks/frame) ---
            val ticks = if (fastForward) 4 else 1
            repeat(ticks) { core.tick() }

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
    }

    // ---------------------------------------------------------------------------
    // Public API — called from any thread; GL-critical work is queued/synced.
    // ---------------------------------------------------------------------------

    /** Queue a system load; it executes on the next [onDrawFrame]. */
    fun queueSystemLoad(iplRomBytes: ByteArray?, boardsBmlBytes: ByteArray) {
        pendingIplRom    = iplRomBytes
        pendingBoardsBml = boardsBmlBytes
        requestRender()
    }

    /**
     * Queue a ROM load; it executes on the next [onDrawFrame] after the system is ready.
     * @param system  ares system ID (e.g. "sfc") — stored for [EmulatorStarted] event.
     * @param romPath Absolute file path — stored for [EmulatorStarted] event.
     */
    fun queueRomLoad(romBytes: ByteArray, system: String = "sfc", romPath: String = "") {
        loadedSystem  = system
        loadedRomPath = romPath
        firstFrameSent = false
        pendingRomBytes = romBytes
        requestRender()
    }

    /** Pause emulation. Audio keeps running; tick() becomes a no-op until [resumeEmulation]. */
    fun pauseEmulation() {
        core.pause()
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
            core.destroy()
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
        queueEvent { core.destroy() }
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
