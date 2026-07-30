package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Regression for the showcase-screen bug: stopEmulation() destroys the core,
 * and the follow-up loadSystem for a different system failed every frame
 * because nothing re-initialized it. Pins the destroy → init → load-another
 * sequence the renderer's stop path now performs.
 */
@RunWith(AndroidJUnit4::class)
class SystemSwitchTest {

    private val context = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun coreSurvivesStopStyleSwitchAcrossAllSystems() {
        val core = EmulatorCore()
        try {
            assert(core.init())
            val systems = core.supportedSystems().split(",")
            var previous: String? = null

            for (system in systems) {
                if (previous != null) {
                    // The stopEmulation() sequence between systems.
                    core.destroy()
                    assert(core.init()) { "re-init after destroy must succeed" }
                }
                assert(core.loadSystem(system)) { "loadSystem($system) after ${previous ?: "fresh init"}" }
                val rom = romFor(system)
                if (rom != null) {
                    assert(core.loadRom(rom, null) == EmulatorCore.LOAD_OK) { "loadRom($system)" }
                    repeat(10) { core.tick() }
                }
                previous = system
            }
        } finally {
            core.destroy()
        }
    }

    /** Homebrew test ROMs pushed by the instrumented test setup, if present. */
    private fun romFor(system: String): ByteArray? {
        val f = java.io.File("/data/local/tmp/test-$system.rom")
        return if (f.exists()) f.readBytes() else null
    }
}
