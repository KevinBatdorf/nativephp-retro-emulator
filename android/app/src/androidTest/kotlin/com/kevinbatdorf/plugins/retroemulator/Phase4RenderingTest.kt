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
 *
 * System firmware (ipl.rom, boards.bml, …) is embedded in the native library
 * since Phase 11 — no extra assets are needed.
 */
@RunWith(AndroidJUnit4::class)
class Phase4RenderingTest {

    private val context = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun emulatorActivityRunsWithoutCrash() {
        val romPath = "/data/local/tmp/test.sfc"

        // Skip rather than fail when no ROM is present (CI without ROM assets).
        val romFile = File(romPath)
        if (!romFile.exists()) {
            android.util.Log.w("Phase4RenderingTest",
                "Skipping: no ROM at $romPath — push a .sfc ROM first")
            return
        }

        val intent = Intent(context, EmulatorActivity::class.java).apply {
            putExtra(EmulatorActivity.EXTRA_ROM_PATH, romPath)
        }

        ActivityScenario.launch<EmulatorActivity>(intent).use { scenario ->
            // Allow 3 seconds of emulation — confirms no crash during startup.
            Thread.sleep(3_000)
            // Tear the core down on the GL thread while it is still alive —
            // ares requires teardown on the thread that loaded the system,
            // and later tests re-init the shared native core.
            scenario.onActivity { it.renderer.stopEmulation() }
            Thread.sleep(1_000)
        }
    }

    @Test
    fun coreInitialisesWithoutCrash() {
        val core = AresCore()
        assert(core.init()) { "nativeInit() must return true" }
        assert(core.version().isNotEmpty()) { "version must be non-empty" }
        core.destroy()
    }

    @Test
    fun supportedSystemsAreReported() {
        // Verify the four Phase 11 systems are compiled into the native library.
        val supported = AresCore().supportedSystems().split(",")
        for (id in listOf("fc", "sfc", "gb", "md")) {
            assert(id in supported) { "system '$id' must be supported, got: $supported" }
        }
    }
}
