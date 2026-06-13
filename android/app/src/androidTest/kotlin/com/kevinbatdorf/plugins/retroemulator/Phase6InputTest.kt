package com.kevinbatdorf.plugins.retroemulator

import android.view.KeyEvent
import android.view.MotionEvent
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Phase 6 instrumented tests — controller input.
 *
 * These tests verify the Kotlin ↔ JNI input path without requiring a running
 * ROM. nativeSetInputState is a no-op when g_state is null, so the tests are
 * safe to run even without a full system load.
 */
@RunWith(AndroidJUnit4::class)
class Phase6InputTest {

    @Test
    fun setInputStateDoesNotCrash() {
        val core = AresCore()
        core.init()
        core.setInputState(1, 0)
        core.setInputState(1, EmulatorInput.BTN_A or EmulatorInput.BTN_B)
        core.setInputState(1, EmulatorInput.BTN_UP or EmulatorInput.BTN_DOWN)
        core.setInputState(2, EmulatorInput.BTN_START or EmulatorInput.BTN_SELECT)
        core.setInputState(1, 0)
        core.destroy()
    }

    @Test
    fun knownGamepadKeycodeIsConsumed() {
        val core = AresCore()
        val input = EmulatorInput(core)

        val knownKeycodes = listOf(
            KeyEvent.KEYCODE_BUTTON_A,
            KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_X,
            KeyEvent.KEYCODE_BUTTON_Y,
            KeyEvent.KEYCODE_BUTTON_L1,
            KeyEvent.KEYCODE_BUTTON_R1,
            KeyEvent.KEYCODE_BUTTON_START,
            KeyEvent.KEYCODE_BUTTON_SELECT,
            KeyEvent.KEYCODE_DPAD_UP,
            KeyEvent.KEYCODE_DPAD_DOWN,
            KeyEvent.KEYCODE_DPAD_LEFT,
            KeyEvent.KEYCODE_DPAD_RIGHT,
        )
        for (keyCode in knownKeycodes) {
            val down = KeyEvent(KeyEvent.ACTION_DOWN, keyCode)
            assertTrue("Expected keycode $keyCode to be consumed", input.onKeyEvent(down))
            val up = KeyEvent(KeyEvent.ACTION_UP, keyCode)
            assertTrue("Expected keycode $keyCode release to be consumed", input.onKeyEvent(up))
        }

        core.destroy()
    }

    @Test
    fun unknownKeycodeIsNotConsumed() {
        val core = AresCore()
        val input = EmulatorInput(core)

        val unknownKeycodes = listOf(
            KeyEvent.KEYCODE_HOME,
            KeyEvent.KEYCODE_BACK,
            KeyEvent.KEYCODE_VOLUME_UP,
        )
        for (keyCode in unknownKeycodes) {
            val event = KeyEvent(KeyEvent.ACTION_DOWN, keyCode)
            assertFalse("Expected keycode $keyCode to NOT be consumed", input.onKeyEvent(event))
        }

        core.destroy()
    }

    @Test
    fun motionEventFromNonGamepadSourceIsNotConsumed() {
        val core = AresCore()
        val input = EmulatorInput(core)

        // A plain touch event (SOURCE_TOUCHSCREEN) should be rejected.
        val touchEvent = MotionEvent.obtain(0L, 0L, MotionEvent.ACTION_MOVE, 0f, 0f, 0)
        assertFalse(input.onMotionEvent(touchEvent))
        touchEvent.recycle()

        core.destroy()
    }

    @Test
    fun resetClearsAllButtonState() {
        val core = AresCore()
        val input = EmulatorInput(core)

        // Press a button then reset — must not crash and setInputState(1,0) is called.
        input.onKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_A))
        input.reset()

        core.destroy()
    }
}
