package com.kevinbatdorf.plugins.retroemulator

import android.content.Intent
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Phase 4 instrumented test — confirms the emulator activity starts and runs
 * for 3 seconds without crashing.
 *
 * Requirements:
 *   - A test ROM must be pushed to the device before running:
 *       adb push /path/to/game.sfc /data/local/tmp/test.sfc
 *   - An optional ipl.rom (64 bytes) may also be pushed:
 *       adb push /path/to/ipl.rom /data/local/tmp/ipl.rom
 *     Without ipl.rom the SMP boots from a zero stub, so the game will not
 *     progress past audio init — but the emulator will not crash.
 *
 * These paths are intentionally hard-coded for the test environment; change
 * them if your setup differs.
 */
@RunWith(AndroidJUnit4::class)
class Phase4RenderingTest {

    private val context = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun emulatorActivityRunsWithoutCrash() {
        val romPath = "/data/local/tmp/test.sfc"
        val iplPath = "/data/local/tmp/ipl.rom"

        // Skip rather than fail when no ROM is present (CI without ROM assets).
        val romFile = File(romPath)
        if (!romFile.exists()) {
            android.util.Log.w("Phase4RenderingTest",
                "Skipping: no ROM at $romPath — push a .sfc ROM first")
            return
        }

        val intent = Intent(context, EmulatorActivity::class.java).apply {
            putExtra(EmulatorActivity.EXTRA_ROM_PATH, romPath)
            if (File(iplPath).exists()) {
                putExtra(EmulatorActivity.EXTRA_IPL_PATH, iplPath)
            }
        }

        ActivityScenario.launch<EmulatorActivity>(intent).use {
            // Allow 3 seconds of emulation — confirms no crash during startup.
            Thread.sleep(3_000)
        }
    }

    @Test
    fun coreInitialisesWithoutCrash() {
        // Smoke-test that the .so loads, init returns true, and destroy is clean.
        val core = AresCore()
        assert(core.init()) { "nativeInit() must return true" }
        assert(core.version().isNotEmpty()) { "version must be non-empty" }
        core.destroy()
    }

    @Test
    fun boardsBmlIsAccessible() {
        // Verify the bundled boards.bml raw resource can be read.
        val bytes = context.resources.openRawResource(R.raw.super_famicom_boards)
            .use { it.readBytes() }
        assert(bytes.isNotEmpty()) { "boards.bml raw resource must be non-empty" }
        assert(bytes.size > 1_000) { "boards.bml should be at least 1 KB" }
    }
}
