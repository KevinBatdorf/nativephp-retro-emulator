package com.kevinbatdorf.plugins.retroemulator

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import java.io.File

/**
 * Test harness activity for rendering verification.
 *
 * Intent extras:
 *   ROM_PATH   (String, required) — absolute path to a ROM file.
 *   SYSTEM     (String, optional) — ares system ID ("sfc", "fc", "gb", "md").
 *              Defaults to "sfc". System firmware is embedded in the native
 *              library; no extra assets are needed.
 *   OUTPUT            (String, optional) — presentation mode: scale (default),
 *                     integer, integerFixed, stretch.
 *   FIXED_SCALE       (Int, optional)    — multiplier for integerFixed.
 *   ASPECT_CORRECTION (String, optional) — none, standard (default), anamorphic.
 *
 * The activity displays an [EmulatorRenderer] full-screen and immediately
 * starts loading once the GL surface is ready.
 */
class EmulatorActivity : Activity() {

    companion object {
        const val EXTRA_ROM_PATH = "ROM_PATH"
        const val EXTRA_SYSTEM   = "SYSTEM"
        const val EXTRA_BIOS_PATH = "BIOS_PATH"
        const val EXTRA_SLOT_A_PATH = "SLOT_A_PATH"
        const val EXTRA_SLOT_B_PATH = "SLOT_B_PATH"
        const val EXTRA_REGION   = "REGION"
        const val EXTRA_SHADER   = "SHADER"
        const val EXTRA_OUTPUT   = "OUTPUT"
        const val EXTRA_FIXED_SCALE = "FIXED_SCALE"
        const val EXTRA_ASPECT_CORRECTION = "ASPECT_CORRECTION"
        const val EXTRA_SCREENSHOT_AFTER_MS = "SCREENSHOT_AFTER_MS"
        private const val TAG = "EmulatorActivity"
    }

    // Internal so instrumented tests can stop emulation deterministically
    // (on the GL thread, while it is still alive) before the scenario closes.
    internal lateinit var renderer: EmulatorRenderer

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val romPath = intent.getStringExtra(EXTRA_ROM_PATH)
        if (romPath == null) {
            Log.e(TAG, "ROM_PATH not provided — finishing")
            finish()
            return
        }
        val system = intent.getStringExtra(EXTRA_SYSTEM) ?: "sfc"

        renderer = EmulatorRenderer(this)
        setContentView(renderer)

        intent.getStringExtra(EXTRA_OUTPUT)?.let { renderer.videoOutput = it }
        renderer.videoFixedScale = intent.getIntExtra(EXTRA_FIXED_SCALE, 2)
        intent.getStringExtra(EXTRA_ASPECT_CORRECTION)?.let { renderer.videoAspectCorrection = it }
        intent.getStringExtra(EXTRA_REGION)?.let { renderer.stagedRegion = it }

        // Queue the system load (executes on GL thread). BIOS_PATH feeds
        // firmware-gated systems (gba) their dev-supplied dump.
        renderer.queueSystemLoad(system, intent.getStringExtra(EXTRA_BIOS_PATH) ?: "")

        val romFile = File(romPath)
        val saveDir = File(filesDir, "saves")
        saveDir.mkdirs()
        val savePrefix = File(saveDir, romFile.nameWithoutExtension).absolutePath
        val romBytes = romFile.readBytes()

        // Stage Sufami Turbo slot carts (A=0, B=1). stageSlot only records the
        // bytes; the renderer inserts them into the core on the render thread
        // right before the boot, so ordering relative to the queued load — and
        // waiting for the core to exist — is handled there.
        intent.getStringExtra(EXTRA_SLOT_A_PATH)?.let { renderer.stageSlot(0, File(it).readBytes()) }
        intent.getStringExtra(EXTRA_SLOT_B_PATH)?.let { renderer.stageSlot(1, File(it).readBytes()) }

        // Queue the ROM load (executes on the render thread after the system is staged).
        renderer.queueRomLoad(romBytes, system, romPath, savePrefix)
        Log.i(TAG, "ROM queued: $romPath (${romBytes.size} bytes, system=$system)")

        // Optional shader for on-device librashader verification. Applied off the
        // main thread after the surface + core are up (the real flow applies
        // shaders through loadSystem's config fan-out after registerSurface).
        intent.getStringExtra(EXTRA_SHADER)?.let { shaderPath ->
            Thread {
                Thread.sleep(2500)
                val ok = renderer.syncSetShader(shaderPath)
                Log.i(TAG, "setShader($shaderPath) → $ok")
            }.start()
        }

        // Bridge-path screenshot after a delay — the on-device proof that
        // Screenshot captures what's presented (post-shader when one is set).
        val shotDelay = intent.getLongExtra(EXTRA_SCREENSHOT_AFTER_MS, 0)
        if (shotDelay > 0) {
            Thread {
                Thread.sleep(shotDelay)
                val png = renderer.syncScreenshot()
                if (png != null) {
                    val f = File(filesDir, "screenshot.png")
                    f.writeBytes(png)
                    Log.i(TAG, "screenshot saved: ${f.absolutePath} (${png.size} bytes)")
                } else {
                    Log.e(TAG, "screenshot returned null")
                }
            }.start()
        }
    }

    override fun onResume()  { super.onResume();  renderer.onResume() }
    override fun onPause()   { super.onPause();   renderer.onPause();  renderer.input.reset() }
    override fun onDestroy() { super.onDestroy(); renderer.release() }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (renderer.input.onKeyEvent(event)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (renderer.input.onMotionEvent(event)) return true
        return super.dispatchGenericMotionEvent(event)
    }
}
