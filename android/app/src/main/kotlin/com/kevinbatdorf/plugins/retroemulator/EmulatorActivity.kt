package com.kevinbatdorf.plugins.retroemulator

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import java.io.File

/**
 * Test harness activity for Phase 4 rendering verification.
 *
 * Intent extras:
 *   ROM_PATH   (String, required) — absolute path to a .sfc ROM file.
 *   IPL_PATH   (String, optional) — absolute path to the 64-byte ipl.rom.
 *              If omitted, an all-zero stub is used (SMP will hang at audio
 *              init but the emulator will not crash during the test window).
 *
 * The activity displays an [EmulatorRenderer] full-screen and immediately
 * starts loading once the GL surface is ready.
 */
class EmulatorActivity : Activity() {

    companion object {
        const val EXTRA_ROM_PATH = "ROM_PATH"
        const val EXTRA_IPL_PATH = "IPL_PATH"
        private const val TAG = "EmulatorActivity"
    }

    private lateinit var renderer: EmulatorRenderer

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val romPath = intent.getStringExtra(EXTRA_ROM_PATH)
        if (romPath == null) {
            Log.e(TAG, "ROM_PATH not provided — finishing")
            finish()
            return
        }

        renderer = EmulatorRenderer(this)
        setContentView(renderer)

        // Load boards.bml from the raw resource bundled with the plugin.
        val boardsBml = resources.openRawResource(R.raw.super_famicom_boards)
            .use { it.readBytes() }

        // Load optional ipl.rom from a user-provided path.
        val iplRomPath = intent.getStringExtra(EXTRA_IPL_PATH)
        val iplRom: ByteArray? = iplRomPath?.let { path ->
            val file = File(path)
            if (file.exists()) file.readBytes() else null
        }

        if (iplRom == null) {
            Log.w(TAG, "ipl.rom not provided — SMP will use zero stub")
        }

        // Queue the system load (executes on GL thread).
        renderer.queueSystemLoad(iplRom, boardsBml)

        // Queue the ROM load (executes on GL thread after system is ready).
        val romBytes = File(romPath).readBytes()
        renderer.queueRomLoad(romBytes)

        Log.i(TAG, "ROM queued: $romPath (${romBytes.size} bytes)")
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
