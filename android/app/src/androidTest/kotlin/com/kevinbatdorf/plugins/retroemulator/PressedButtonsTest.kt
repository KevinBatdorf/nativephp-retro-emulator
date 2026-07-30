package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Reading held buttons back by NAME. The on-screen pad mirrors this so a
 * hardware controller lights the same arm a finger would; a bare bitmask can't
 * serve that, since each system numbers its buttons differently.
 */
@RunWith(AndroidJUnit4::class)
class PressedButtonsTest {

    private fun pressed(core: EmulatorCore, port: Int = 1) =
        core.getPressedButtons(port).split(',').filter { it.isNotEmpty() }.toSet()

    @Test
    fun reportsSoftwarePressesByName() {
        val core = EmulatorCore()
        try {
            assertTrue(core.init())
            assertTrue(core.loadSystem("sfc"))
            assertEquals(emptySet<String>(), pressed(core))

            assertEquals("", core.pressButton(1, "Up", true))
            assertEquals(setOf("Up"), pressed(core))

            assertEquals("", core.pressButton(1, "Right", true))
            assertEquals(setOf("Up", "Right"), pressed(core))
        } finally {
            core.destroy()
        }
    }

    @Test
    fun reportsNothingForAnEmptyPort() {
        val core = EmulatorCore()
        try {
            assertTrue(core.init())
            assertTrue(core.loadSystem("sfc"))

            assertEquals(emptySet<String>(), pressed(core, port = 2))
        } finally {
            core.destroy()
        }
    }

    @Test
    fun reportsNothingBeforeASystemIsLoaded() {
        val core = EmulatorCore()
        try {
            assertTrue(core.init())

            assertEquals(emptySet<String>(), pressed(core))
        } finally {
            core.destroy()
        }
    }

    /** Names come from the port's own device, not a fixed d-pad list. */
    @Test
    fun namesFollowTheConnectedDevice() {
        val core = EmulatorCore()
        try {
            assertTrue(core.init())
            assertTrue(core.loadSystem("sfc"))
            assertEquals("", core.connectDevice("sfc", 2, "Mouse"))

            assertEquals("", core.pressButton(2, "Left", true))
            assertEquals(setOf("Left"), pressed(core, port = 2))
        } finally {
            core.destroy()
        }
    }
}
