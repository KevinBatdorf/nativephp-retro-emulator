package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertNull
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Regression: reopening the same game after stop() left a black screen. The session
 * kept the last declarative boot key, so the remounted element saw no change and
 * never booted.
 */
@RunWith(AndroidJUnit4::class)
class RelaunchTest {

    @Test
    fun stoppingForgetsTheDeclaredBootSoTheSameRomBootsAgain() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val session = EmulatorSession(context)
        session.declaredBootKey = "sfc {} /cache/game.sfc"

        session.stopEmulation()

        assertNull(session.declaredBootKey)
    }
}
