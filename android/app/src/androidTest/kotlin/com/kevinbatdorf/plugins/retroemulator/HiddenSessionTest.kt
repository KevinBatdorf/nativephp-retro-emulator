package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Regression: with no surface the render loop idled without serving requests, so a
 * save asked for as the player left the app blocked its caller for the full 30 s.
 */
@RunWith(AndroidJUnit4::class)
class HiddenSessionTest {

    @Test
    fun aSessionWithNoSurfaceAnswersStateAndScreenshotRequestsAtOnce() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val session = EmulatorSession(context)
        val path = java.io.File(context.cacheDir, "hidden.state").path

        val started = System.nanoTime()
        session.syncStateSave(path)
        val frame = session.syncScreenshot()
        val seconds = (System.nanoTime() - started) / 1e9

        assertTrue("took ${seconds}s", seconds < 2.0)
        assertNull(frame)
    }
}
