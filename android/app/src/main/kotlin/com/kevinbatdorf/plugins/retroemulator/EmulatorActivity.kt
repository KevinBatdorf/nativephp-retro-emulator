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
 *
 * The activity displays an [EmulatorRenderer] full-screen and immediately
 * starts loading once the GL surface is ready.
 */
class EmulatorActivity : Activity() {

    companion object {
        const val EXTRA_ROM_PATH = "ROM_PATH"
        const val EXTRA_SYSTEM   = "SYSTEM"
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

        // Queue the system load (executes on GL thread).
        renderer.queueSystemLoad(system)

        // Queue the ROM load (executes on GL thread after system is ready).
        val romFile = File(romPath)
        val saveDir = File(filesDir, "saves")
        saveDir.mkdirs()
        val savePrefix = File(saveDir, romFile.nameWithoutExtension).absolutePath
        val romBytes = romFile.readBytes()
        renderer.queueRomLoad(romBytes, system, romPath, savePrefix)

        Log.i(TAG, "ROM queued: $romPath (${romBytes.size} bytes, system=$system)")
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
