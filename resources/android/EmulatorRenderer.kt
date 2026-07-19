package com.kevinbatdorf.plugins.retroemulator

import android.content.Context
import android.graphics.Bitmap
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

private const val TAG = "EmulatorRenderer"

// Emulation pacing fallback, used only until the core's first
// Platform::refreshRateHint arrives during system power-on. After that the
// hint is authoritative — per-system and per-region (SFC NTSC 60.0988,
// GB 59.7275, MD NTSC 59.9228, PAL ~50), polled per frame because some cores
// re-hint on video-mode changes.
private const val FALLBACK_FPS = 60.0988
private const val MAX_TICKS_PER_FRAME = 8

// Battery-save auto-flush cadence — matches ares' desktop 30 s auto-save.
private const val AUTOSAVE_INTERVAL_NANOS = 30_000_000_000L

// Motors publish on/off transitions, not a sustain signal — a long one-shot
// stands in for "held" and the zero transition cancels it.
private const val RUMBLE_ONESHOT_MS = 10_000L

/**
 * A [SurfaceView] that drives the ares emulator and presents each frame through
 * the native Vulkan renderer (librashader-capable). A dedicated render thread
 * owns the one thread every ares call runs on, so libco coroutine contexts stay
 * consistent for the whole session.
 *
 * Usage:
 * 1. Construct and set as the Activity's content view (or wrap in AndroidView).
 * 2. Call [queueSystemLoad] / [queueRomLoad]; the native calls execute on the
 *    render thread once the surface is ready.
 */
class EmulatorRenderer(context: Context) : SurfaceView(context), SurfaceHolder.Callback {

    private val core  = AresCore()
    private val audio = EmulatorAudio(core)
    val input = EmulatorInput(core)

    // Set when this surface installs a global gamepad capturer on the host
    // window (input-capture="global"); invoked on release to restore the
    // original window callback.
    var windowCaptureRestore: (() -> Unit)? = null

    // Last declarative setup (system/config/rom) applied on mount, so a
    // recomposition only re-stages when the element's props actually change.
    var declaredBootKey: String? = null

    private val vibrator: Vibrator? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        (context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager)?.defaultVibrator
    } else {
        @Suppress("DEPRECATION")
        context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
    }

    @Volatile private var lastRumbleState = 0

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
    @Volatile var pendingBiosPath: String?    = null
    @Volatile var pendingRomBytes: ByteArray? = null
    @Volatile var pendingSavePrefix: String?  = null

    // Staged system declaration (plan 4b) — LoadSystem never boots a core;
    // these carry the declaration until a ROM arrives and triggers the boot.
    @Volatile var stagedSystemId: String = ""
        private set
    @Volatile var stagedRegion: String = ""            // explicit override, "" = auto
    @Volatile var stagedPreferredRegions: String = ""  // CSV, "" = desktop default NTSC-U

    @Volatile private var systemStaged = false
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
    // Requests are posted from bridge threads and serviced on the render thread.

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
    // The native side mirrors the flag so run-ahead suppresses itself.
    @Volatile var fastForward: Boolean = false
        set(value) {
            field = value
            core.setFastForward(value)
        }
    @Volatile var speedMultiplier: Double = 1.0

    // Frame pacing — the render loop presents at the display's refresh rate
    // (Vulkan FIFO/vsync; 120 Hz on some devices), so ticks are budgeted by wall
    // clock against the console's native frame rate rather than per present.
    private var lastTickNanos = 0L
    private var tickAccumulator = 0.0

    // Phase 13 — periodic battery-save flush (30 s, matching ares' desktop
    // auto-save cadence). Set false via LoadSystem config { autoSave: false }.
    @Volatile var autoSave: Boolean = true
    private var lastAutoSaveNanos = 0L

    // Letterboxed output rect in surface pixels, recomputed per frame from the
    // screen-node geometry (GL thread only). Screenshot reads this region.
    @Volatile private var surfaceW = 0
    @Volatile private var surfaceH = 0
    private var outputRect = OutputRect(0, 0, 0, 0)

    // Presentation settings (ares desktop's Video settings) — written from
    // bridge threads via [queueVideoOptions], read per frame on the GL thread.
    @Volatile var videoOutput: String = "scale"
    @Volatile var videoFixedScale: Int = 2
    @Volatile var videoAspectCorrection: String = "standard"

    // Current screen-node options, merged on the GL thread. Kept so setVideo
    // has merge semantics (omitted options hold their value) and so a system
    // reload can reapply them — desktop's settings persist the same way.
    private class VideoOptions {
        var luminance = 1f
        var saturation = 1f
        var gamma = 1f
        var colorBleed = false
        var overscan = false
    }
    private val videoOptions = VideoOptions()

    private fun applyVideoOptions() = core.setVideo(
        videoOptions.luminance, videoOptions.saturation, videoOptions.gamma,
        videoOptions.colorBleed, videoOptions.overscan,
    )

    // Per-system emulation toggles (Color Emulation, Deep Black Boost,
    // Interframe Blending). Default off — applied even when unset, since the
    // cores default them on; a fresh core reapplies these like videoOptions.
    // Unsupported keys no-op natively, so every system carries the full map.
    private val coreOptions = linkedMapOf(
        "colorEmulation" to false,
        "deepBlackBoost" to false,
        "interframeBlending" to false,
        "showIcons" to false,
    )

    private fun applyCoreOptions() = coreOptions.forEach { (key, value) ->
        core.setCoreBoolean(key, value)
    }

    // -----------------------------------------------------------------------
    // Render thread — replaces GLSurfaceView's managed GL thread. It owns the
    // single thread every ares call runs on (libco), drains queued commands,
    // ticks the core paced to its refresh rate, and presents each frame through
    // the native Vulkan renderer. The Vulkan swapchain follows the SurfaceHolder.
    // -----------------------------------------------------------------------
    private val events = ConcurrentLinkedQueue<Runnable>()
    private val lifecycleLock = Object()
    @Volatile private var renderThread: RenderThread? = null
    @Volatile private var currentSurface: Surface? = null
    @Volatile private var threadPaused = false

    // Shader staging: a declarative boot fans SetShader out before the render
    // thread has bound the first Vulkan surface (g_vk doesn't exist yet), so
    // an immediate native call can only fail. syncSetShader stages the path
    // instead and the render thread applies it right after the bind.
    @Volatile private var vkBound = false
    @Volatile private var wantedShaderPath: String? = null
    @Volatile private var shaderPending = false

    /** GLSurfaceView-style post: run [r] on the render thread (serviced each loop). */
    fun queueEvent(r: Runnable) {
        events.add(r)
        synchronized(lifecycleLock) { lifecycleLock.notifyAll() }
    }

    /**
     * Wake the render loop. It renders continuously, so this only nudges it out
     * of an idle wait (no surface / paused). Kept so call sites that used
     * GLSurfaceView.requestRender() still compile and behave.
     */
    fun requestRender() {
        synchronized(lifecycleLock) { lifecycleLock.notifyAll() }
    }

    private inner class RenderThread : Thread("EmulatorRender") {
        @Volatile var running = true
        private var boundSurface: Surface? = null

        override fun run() {
            // ares platform first; the Vulkan device comes up lazily when a
            // surface binds (core.surfaceCreated) and survives core stop/init.
            core.init()
            Log.i(TAG, "render thread started — ares ${core.version()}")
            try {
                while (running) {
                    while (true) { (events.poll() ?: break).run() }

                    // Bind / rebind / unbind the Vulkan surface on this thread.
                    val want = currentSurface
                    if (want !== boundSurface) {
                        if (boundSurface != null) core.surfaceDestroyed()
                        boundSurface = want
                        if (want != null) core.surfaceCreated(want)
                        vkBound = want != null
                    }

                    // Apply a shader staged before the first bind (see the
                    // field comment). Failures surface on the event channel —
                    // there is no bridge call left to answer.
                    if (vkBound && shaderPending) {
                        shaderPending = false
                        val path = wantedShaderPath
                        if (!core.setShader(path) && path != null) {
                            eventListener?.onError(
                                "SHADER_FAILED",
                                "Failed to load shader preset '$path'",
                            )
                        }
                    }

                    // Idle when there is nothing to draw, but keep draining
                    // events (release()/teardown must still run with no surface).
                    if (boundSurface == null || threadPaused) {
                        synchronized(lifecycleLock) {
                            if (running && events.isEmpty() &&
                                currentSurface === boundSurface &&
                                (boundSurface == null || threadPaused)) {
                                lifecycleLock.wait(250)
                            }
                        }
                        continue
                    }
                    doFrame()
                }
            } finally {
                if (boundSurface != null) core.surfaceDestroyed()
                core.releaseRenderer()
                Log.i(TAG, "render thread stopped")
            }
        }
    }

    private fun doFrame() {
            // --- Consume pending system staging (never boots — plan 4b) ---
            val systemId = pendingSystemId
            if (systemId != null) {
                // Consume on success too: a stale request left behind would
                // silently re-stage on the frame after a stop.
                pendingSystemId = null
                systemStaged = core.loadSystem(systemId, pendingBiosPath)
                if (!systemStaged) Log.e(TAG, "loadSystem($systemId) failed")
            }

            // --- Service pending ports read here so it orders after a queued staging ---
            pendingPortsRead.getAndSet(null)?.let { req ->
                req.result = if (systemStaged) core.getPortsJson() else null
                req.latch.countDown()
            }

            // --- Consume pending ROM load — the one boot path, swaps included ---
            val rom = pendingRomBytes
            if (systemStaged && rom != null) {
                pendingRomBytes = null
                when (core.loadRom(rom, pendingSavePrefix, stagedRegion, stagedPreferredRegions)) {
                    AresCore.LOAD_OK -> {
                        romLoaded = true
                        currentStatus = "loading"
                        // Fresh screen nodes boot with ares defaults — reapply
                        // the surface's options like desktop reapplies its
                        // settings. (Core-side prefs — volume, DRC, run-ahead,
                        // rewind config, rumble — live in g_state atomics that
                        // survive the in-place reboot.)
                        applyVideoOptions()
                        applyCoreOptions()
                    }
                    AresCore.LOAD_REJECTED -> {
                        // Pre-teardown rejection: a running game is untouched.
                        Log.e(TAG, "loadRom rejected — prior state kept")
                        eventListener?.onError("LOAD_FAILED", "ROM rejected by analyzer")
                    }
                    AresCore.LOAD_BIOS_REQUIRED -> {
                        Log.e(TAG, "loadRom rejected — system firmware missing")
                        eventListener?.onError("BIOS_REQUIRED",
                            "this system needs firmware — pass biosPath in the LoadSystem config")
                    }
                    AresCore.LOAD_FAILED_STOPPED -> {
                        romLoaded = false
                        audioStarted = false
                        audio.stop()
                        currentStatus = "stopped"
                        Log.e(TAG, "loadRom failed after teardown — emulator stopped")
                        eventListener?.onError("LOAD_FAILED", "boot failed; emulator stopped")
                    }
                }
            }

            // --- Start audio once the ROM is running ---
            if (romLoaded && !audioStarted) {
                audioStarted = true
                audio.start()
            }

            if (!romLoaded) {
                // No game loaded: present a cleared (black) frame so the surface
                // shows black rather than a stale image, then idle this pass.
                core.presentFrame(0, 0, 0, 0)
                return
            }

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
            val hint = core.refreshRateHint()
            val targetFps = if (hint > 0.0) hint else FALLBACK_FPS
            tickAccumulator += elapsed * targetFps * speed
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

            // --- Update the letterboxed output rect ---
            if (fw > 0 && fh > 0) {
                val g = core.getVideoGeometry()
                outputRect = computeOutputRect(
                    nodeWidth = g[0], nodeHeight = g[1],
                    scaleX = g[2], scaleY = g[3],
                    aspectX = g[4], aspectY = g[5],
                    rotation = g[6].toInt(),
                    viewportWidth = surfaceW, viewportHeight = surfaceH,
                    output = videoOutput,
                    fixedScale = videoFixedScale,
                    aspectCorrection = videoAspectCorrection,
                )
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

            // --- Service pending screenshot (after the rect update above) ---
            pendingScreenshot.getAndSet(null)?.let { req ->
                req.result = captureFramebuffer()
                req.latch.countDown()
            }

            // --- Drive the vibrator from ares' motor state ---
            val rumble = core.rumbleState()
            if (rumble != lastRumbleState) {
                lastRumbleState = rumble
                applyRumble(rumble)
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

            // Present the frame, letterboxed into the output rect, via Vulkan.
            // The native blit scales the source with a linear filter and centers
            // it in outputRect (the clear paints the letterbox bars) — the exact
            // ruby presentation, done by vkCmdBlitImage instead of a quad.
            core.presentFrame(outputRect.x, outputRect.y, outputRect.w, outputRect.h)
    }

    // -----------------------------------------------------------------------
    // SurfaceHolder lifecycle — drives the render thread + Vulkan swapchain.
    // -----------------------------------------------------------------------
    init {
        holder.addCallback(this)
        // Gamepad input doesn't reset Android's idle timer — keep the display
        // awake while the emulator view is showing.
        keepScreenOn = true
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        currentSurface = holder.surface
        val t = renderThread
        if (t == null || !t.isAlive) {
            renderThread = RenderThread().also { it.start() }
        } else {
            requestRender()
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        currentSurface = holder.surface
        queueEvent {
            surfaceW = width
            surfaceH = height
            core.surfaceChanged(width, height)
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // The Surface is invalid the moment this returns, so block until the
        // render thread has torn down its swapchain + released the native window.
        currentSurface = null
        val latch = CountDownLatch(1)
        queueEvent {
            core.surfaceDestroyed()
            latch.countDown()
        }
        requestRender()
        if (!latch.await(2, TimeUnit.SECONDS)) {
            Log.w(TAG, "surfaceDestroyed: render thread didn't release the surface in time")
        }
    }

    /** Pause emulation while backgrounded (Activity.onPause) — flushes battery saves. */
    fun onPause() {
        threadPaused = true
        queueEvent { core.flushSaves() }
        requestRender()
    }

    /** Resume emulation after [onPause] (Activity.onResume). */
    fun onResume() {
        threadPaused = false
        requestRender()
    }

    // ---------------------------------------------------------------------------
    // Public API — called from any thread; GL-critical work is queued/synced.
    // ---------------------------------------------------------------------------

    /**
     * Queue a system STAGING; it executes on the next render-loop pass. No core
     * boots until a ROM arrives (plan 4b) — re-staging over a running core is
     * legal and leaves the running game untouched until the next ROM load.
     * @param systemId ares system ID — one of [AresCore.supportedSystems].
     */
    fun queueSystemLoad(systemId: String, biosPath: String? = null) {
        stagedSystemId = systemId
        pendingBiosPath = biosPath
        pendingSystemId = systemId
        requestRender()
    }

    /**
     * Queue a ROM load; it executes on the next render-loop pass after a system
     * is staged. This is the one boot path — a running game is torn down and
     * a fresh core boots with the region resolved from this ROM.
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
        // Game knowledge dies with the old game (plan 4b): cheat addresses and
        // watched addresses belong to the outgoing ROM. Cheats clear natively
        // inside the reboot; watches are wrapper-held so clear them here.
        watchedAddresses.clear()
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
        // A held one-shot would keep buzzing while the game is frozen.
        lastRumbleState = 0
        vibrator?.cancel()
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
        lastRumbleState = 0
        vibrator?.cancel()
        queueEvent {
            core.flushSaves()
            core.destroy()
            // Restore the surface-created invariant (core initialized) so a
            // follow-up loadSystem works — system switching goes through here.
            core.init()
            romLoaded     = false
            systemStaged  = false
            audioStarted  = false
            firstFrameSent = false
        }
        // Status flips with the event, not with the queued teardown — a
        // GetStatus right after EmulatorStopped must already say "stopped"
        // (iOS sets it synchronously too; the conformance runner asserts it).
        currentStatus = "stopped"
        eventListener?.onStopped()
    }

    /**
     * Stop audio and tear down the ares core. Call from [Activity.onDestroy].
     * Blocks (≤2 s) until the GL thread has actually destroyed the core:
     * release() runs at teardown, when the GL thread may already be paused or
     * exiting — a fire-and-forget destroy can execute late or never, leaving
     * the global core alive with libco contexts primed on the dying thread.
     * The next surface's loadSystem then drives those coroutines from a
     * different thread, which wedges or crashes the core (co_switch storm;
     * found via the instrumented suite hanging cross-test).
     */
    fun release() {
        watchedAddresses.clear()
        input.reset()
        audio.stop()
        lastRumbleState = 0
        vibrator?.cancel()
        currentSurface = null
        val t = renderThread ?: return
        val latch = CountDownLatch(1)
        queueEvent {
            core.flushSaves()
            core.destroy()
            latch.countDown()
        }
        if (!latch.await(2, TimeUnit.SECONDS)) {
            Log.w(TAG, "release(): render thread never serviced core teardown — " +
                "native core may leak until process death")
        }
        // Stop the render thread; its finally block tears down the Vulkan device.
        t.running = false
        requestRender()
        t.join(2000)
        renderThread = null
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

    // ---------------------------------------------------------------------------
    // Rewind / run-ahead — state lives GL-side (read inside tick)
    // ---------------------------------------------------------------------------

    /** Enable/disable rewind capture (see [AresCore.configureRewind]). Fire-and-forget. */
    fun queueConfigureRewind(enabled: Boolean, bufferSeconds: Int = 0) =
        queueEvent { core.configureRewind(enabled, bufferSeconds) }

    /**
     * Enter/exit rewind playback. Blocks ≤2 s.
     * Returns 1 rewinding, 0 playing, -1 rewind not enabled, null on timeout.
     */
    fun syncToggleRewind(): Int? = syncOnGlThread { core.toggleRewind() }

    /** Enable/disable one-frame run-ahead (see [AresCore.setRunAhead]). Fire-and-forget. */
    fun queueSetRunAhead(enabled: Boolean) = queueEvent { core.setRunAhead(enabled) }

    /** Enable/disable dynamic rate control (see [AresCore.setDynamicRateControl]). Fire-and-forget. */
    fun queueSetDynamicRateControl(enabled: Boolean) =
        queueEvent { core.setDynamicRateControl(enabled) }

    // ---------------------------------------------------------------------------
    // Rumble — ares motor state polled per frame, driven onto the device vibrator
    // ---------------------------------------------------------------------------

    /** Gate rumble forwarding from ares' motor nodes. Safe from any thread. */
    fun setRumbleEnabled(enabled: Boolean) {
        core.setRumbleEnabled(enabled)
        if (!enabled) {
            lastRumbleState = 0
            vibrator?.cancel()
        }
    }

    /** Whether this device can rumble at all. */
    fun hasVibrator(): Boolean = vibrator?.hasVibrator() == true

    /**
     * Merge a per-port controller remap (see [AresCore.setInputMapping]).
     * Native stores it under a lock and applies it on the render thread, so this
     * is safe to call from the bridge thread. Returns "" on success or a
     * category-A error string ("SYSTEM_NOT_LOADED" / "INVALID_PARAMETERS" /
     * "UNKNOWN_BUTTON:<name>").
     */
    fun setInputMapping(port: Int, emulated: Array<String>, source: Array<String>): String =
        core.setInputMapping(port, emulated, source)

    /** Test seam: the positional bit a core button currently reads (see [AresCore.getButtonBit]). */
    fun getButtonBit(port: Int, name: String): Int = core.getButtonBit(port, name)

    /**
     * Register/swap the device on a port (see [AresCore.connectDevice]). Passes
     * the synchronously-known staged system id so validation doesn't race the
     * async LoadSystem staging. Thread-safe.
     */
    fun connectDevice(port: Int, device: String): String =
        core.connectDevice(stagedSystemId, port, device)

    /** Logical ports a physical port's device occupies (see [AresCore.devicePorts]). */
    fun devicePorts(port: Int): IntArray = core.devicePorts(stagedSystemId, port)

    /** Set/clear a software button on a port (see [AresCore.pressButton]). Thread-safe. */
    fun pressButton(port: Int, name: String, down: Boolean): String =
        core.pressButton(port, name, down)

    /** Accumulate a relative axis delta on a port (see [AresCore.setAxis]). Thread-safe. */
    fun setAxis(port: Int, name: String, value: Int): String = core.setAxis(port, name, value)

    /** Aim a light-gun at a normalized position (see [AresCore.aimAt]). Thread-safe. */
    fun aimAt(port: Int, x: Float, y: Float): String = core.aimAt(port, x, y)

    /** Stage a Sufami Turbo slot ROM for the next load (see [AresCore.stageSlot]). */
    fun stageSlot(index: Int, rom: ByteArray) = core.stageSlot(index, rom)

    /** Test seam: pending accumulated axis delta (see [AresCore.getAxisAccum]). */
    fun getAxisAccum(port: Int, name: String): Int = core.getAxisAccum(port, name)

    private fun applyRumble(state: Int) {
        val v = vibrator ?: return
        if (state == 0) {
            v.cancel()
            return
        }
        val strong = (state ushr 16) and 0xFFFF
        val weak = state and 0xFFFF
        val amplitude = (maxOf(strong, weak) * 255 / 65535).coerceIn(1, 255)
        val effect = if (v.hasAmplitudeControl()) {
            VibrationEffect.createOneShot(RUMBLE_ONESHOT_MS, amplitude)
        } else {
            VibrationEffect.createOneShot(RUMBLE_ONESHOT_MS, VibrationEffect.DEFAULT_AMPLITUDE)
        }
        v.vibrate(effect)
    }

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

    /**
     * Load ([path]) or clear (null) a librashader preset on the render thread,
     * where the Vulkan filter chain lives. Own timeout (≤10 s) because the first
     * load compiles the preset's passes. Returns false on a load error, null on
     * timeout.
     */
    fun syncSetShader(path: String?): Boolean? {
        wantedShaderPath = path
        if (!vkBound) {
            // No Vulkan surface yet — stage it; the render thread applies on
            // bind and reports SHADER_FAILED itself if the preset won't load.
            shaderPending = true
            return true
        }
        shaderPending = false
        val latch = CountDownLatch(1)
        var result = false
        queueEvent {
            result = core.setShader(path)
            latch.countDown()
        }
        requestRender()
        return if (latch.await(10, TimeUnit.SECONDS)) result else null
    }

    // ---------------------------------------------------------------------------
    // Phase 7 — memory watches
    // ---------------------------------------------------------------------------

    /**
     * Add or merge address watches. Each entry is either a plain bus address (Int)
     * or a map with "address" and "length" keys. Watches are game knowledge and
     * clear automatically when a new ROM loads (plan 4b).
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
    // Render-thread helpers (called from doFrame)
    // ---------------------------------------------------------------------------

    private fun captureFramebuffer(): ByteArray? {
        val dims = IntArray(2)
        val rgba = core.screenshotRGBA(dims) ?: return null
        // Raw path: the core frame's size; shader path: the post-shader
        // output's size — either way the bytes' own dimensions.
        val fw = dims[0]
        val fh = dims[1]
        if (fw <= 0 || fh <= 0) return null
        val rect = outputRect
        return try {
            // Native hands back the presented frame as top-down RGBA8 (no GL
            // bottom-left flip). Scale it to the letterboxed on-screen content
            // size so the screenshot matches the presented image (aspect-
            // corrected, surface-scaled).
            val frame = Bitmap.createBitmap(fw, fh, Bitmap.Config.ARGB_8888)
            frame.copyPixelsFromBuffer(ByteBuffer.wrap(rgba))
            val bmp = if (rect.w > 0 && rect.h > 0) {
                Bitmap.createScaledBitmap(frame, rect.w, rect.h, true)
            } else {
                frame
            }
            val bos = ByteArrayOutputStream()
            bmp.compress(Bitmap.CompressFormat.PNG, 90, bos)
            if (bmp !== frame) bmp.recycle()
            frame.recycle()
            bos.toByteArray()
        } catch (e: Exception) {
            Log.e(TAG, "Screenshot failed: ${e.message}")
            null
        }
    }

    // ---------------------------------------------------------------------------
    // Phase 14 — audio / video options
    // ---------------------------------------------------------------------------

    // Current audio mix, merged per-key so setVolume() and setBalance() don't
    // clobber each other — each call updates only the knobs it carries.
    @Volatile private var audioVolume = 1.0f
    @Volatile private var audioBalance = 0.0f

    /** Master volume (0–1) and stereo balance (−1 … +1); null keeps the current value. */
    fun setAudioOptions(volume: Float? = null, balance: Float? = null) {
        volume?.let { audioVolume = it }
        balance?.let { audioBalance = it }
        core.setAudio(audioVolume, audioBalance)
    }

    /**
     * Merge video options onto the GL thread — null keeps the current value.
     * Presentation settings (output/fixedScale/aspectCorrection) apply on
     * the next frame; screen-node options need a loaded system and are
     * reapplied automatically when a new system loads.
     */
    fun queueVideoOptions(
        luminance: Float? = null,
        saturation: Float? = null,
        gamma: Float? = null,
        colorBleed: Boolean? = null,
        overscan: Boolean? = null,
        output: String? = null,
        fixedScale: Int? = null,
        aspectCorrection: String? = null,
    ) {
        output?.let { videoOutput = it }
        fixedScale?.let { videoFixedScale = it }
        aspectCorrection?.let { videoAspectCorrection = it }
        queueEvent {
            luminance?.let { videoOptions.luminance = it }
            saturation?.let { videoOptions.saturation = it }
            gamma?.let { videoOptions.gamma = it }
            colorBleed?.let { videoOptions.colorBleed = it }
            overscan?.let { videoOptions.overscan = it }
            applyVideoOptions()
        }
    }

    /**
     * Merge per-system emulation toggles onto the GL thread. Unknown keys are
     * ignored; recognised ones update the persisted map and apply live (a no-op
     * on cores that don't declare the node, and reapplied on the next boot).
     */
    fun queueCoreOptions(options: Map<String, Boolean>) {
        queueEvent {
            options.forEach { (key, value) ->
                if (coreOptions.containsKey(key)) {
                    coreOptions[key] = value
                    if (systemStaged) core.setCoreBoolean(key, value)
                }
            }
        }
    }

    /**
     * Region of the loaded ROM (e.g. "NTSC", "PAL"). Safe to call from any thread
     * once the ROM is loaded — the native value is written before [romLoaded] is set.
     */
    fun getRegion(): String = core.getRegion()

    /** Screen-node presentation geometry (see [AresCore.getVideoGeometry]). Any thread. */
    fun videoGeometry(): DoubleArray = core.getVideoGeometry()

    /**
     * JSON array of controller ports with button names. Registry data — safe
     * from any thread once a system is staged; no booted core required.
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
// Presentation geometry
// ---------------------------------------------------------------------------

/** Letterboxed output rect in surface pixels (GL origin, bottom-left). */
internal data class OutputRect(val x: Int, val y: Int, val w: Int, val h: Int)

/**
 * Port of ares desktop-ui/program/platform.cpp:95-166: the emulated video
 * size is width·scaleX (·aspectX/aspectY unless aspectCorrection "none",
 * ·4/3 more for "anamorphic") × height·scaleY, dimensions swapped for
 * 90°/270° rotation, then sized per output mode — "scale" best-fit,
 * "integer" largest whole multiple, "integerFixed" exactly [fixedScale]×,
 * "stretch" fill — and centered (ruby::video.output centering). Modes that
 * don't fit fall back the way desktop does: integer → best-fit when even 1×
 * overflows; integerFixed → largest fitting multiple, then best-fit.
 * u32 truncation at each step mirrors the reference exactly.
 */
internal fun computeOutputRect(
    nodeWidth: Double, nodeHeight: Double,
    scaleX: Double, scaleY: Double,
    aspectX: Double, aspectY: Double,
    rotation: Int,
    viewportWidth: Int, viewportHeight: Int,
    output: String = "scale",
    fixedScale: Int = 2,
    aspectCorrection: String = "standard",
): OutputRect {
    var videoWidth  = (nodeWidth * scaleX).toInt()
    var videoHeight = (nodeHeight * scaleY).toInt()
    if (aspectCorrection != "none" && aspectY > 0) {
        videoWidth = (videoWidth * aspectX / aspectY).toInt()
    }
    if (aspectCorrection == "anamorphic") videoWidth = videoWidth * 4 / 3
    if (rotation == 90 || rotation == 270) {
        val swap = videoWidth
        videoWidth = videoHeight
        videoHeight = swap
    }
    if (videoWidth <= 0 || videoHeight <= 0 || viewportWidth <= 0 || viewportHeight <= 0) {
        return OutputRect(0, 0, 0, 0)
    }

    val multiplierX = viewportWidth / videoWidth
    val multiplierY = viewportHeight / videoHeight
    val multiplier  = minOf(multiplierX, multiplierY)
    val bestFitScale = minOf(
        viewportWidth.toFloat() / videoWidth,
        viewportHeight.toFloat() / videoHeight,
    )

    var outputWidth  = videoWidth * multiplier
    var outputHeight = videoHeight * multiplier

    if (multiplier == 0 || output == "scale") {
        outputWidth  = (videoWidth * bestFitScale).toInt()
        outputHeight = (videoHeight * bestFitScale).toInt()
    }
    // "integer" keeps video·multiplier from above; the reference's inner
    // fallback is unreachable there (multiplier == 0 is caught first).

    if (output == "integerFixed") {
        var fixedMult = maxOf(1, fixedScale)
        if (fixedMult > multiplierX || fixedMult > multiplierY) {
            fixedMult = maxOf(1, minOf(multiplierX, multiplierY))
            if (multiplierX == 0 || multiplierY == 0) {
                outputWidth  = (videoWidth * bestFitScale).toInt()
                outputHeight = (videoHeight * bestFitScale).toInt()
            } else {
                outputWidth  = videoWidth * fixedMult
                outputHeight = videoHeight * fixedMult
            }
        } else {
            outputWidth  = videoWidth * fixedMult
            outputHeight = videoHeight * fixedMult
        }
    }

    if (output == "stretch") {
        outputWidth  = viewportWidth
        outputHeight = viewportHeight
    }

    return OutputRect(
        (viewportWidth - outputWidth) / 2,
        (viewportHeight - outputHeight) / 2,
        outputWidth,
        outputHeight,
    )
}

// Frame presentation now lives entirely in the native Vulkan renderer
// (vulkan_renderer.cpp): the ares BGRA frame uploads to a B8G8R8A8_UNORM source
// image and vkCmdBlitImage scales it into the letterboxed output rect. No GL
// shader program is needed here anymore — librashader handles slang presets.
